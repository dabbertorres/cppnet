#include "http/http2_connection.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <compare>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <spdlog/logger.h>

#include "coro/notifier.hpp"
#include "coro/task.hpp"
#include "http/connection.hpp"
#include "http/headers.hpp"
#include "http/hpack.hpp"
#include "http/http2.hpp"
#include "http/http2_config.hpp"
#include "http/http2_session.hpp"
#include "http/priority.hpp"
#include "io/io.hpp"
#include "io/reader.hpp"
#include "io/scheduler.hpp"
#include "tcp.hpp"
#include "util/defer.hpp"
#include "util/string_util.hpp"

namespace net::http::detail
{

namespace
{

    // How much is read from the connection at once.
    constexpr std::size_t read_size = std::size_t{32} * 1'024;

    // How much a stream's tunnel buffers each way (see stream_tunnel): the stream has buffers of its own (what's
    // received, and the connection's output), so this only gathers small writes (e.g. a WebSocket's frame header and a
    // short message) into one DATA frame, and small reads out of the stream's lock.
    constexpr std::size_t tunnel_buffer_size = std::size_t{4} * 1'024;

    // A tunnel's stream's window, each way: what its peer may send that its reader hasn't read yet. A stream's initial
    // window (64 KiB) is what a request's body needs, but a tunnel carries a protocol's messages (e.g. a WebSocket's),
    // one after another, as TCP would, whose receive buffer starts at 128 KiB (on Linux and macOS) and grows: so a
    // message of 64 KiB goes without waiting for a WINDOW_UPDATE, and messages on the way keep going while the window's
    // given back. What a peer that doesn't read can make the other side hold is still bounded: by this, and by the
    // connection's window, which bounds all the streams' together.
    constexpr std::uint32_t tunnel_window = std::uint32_t{256} * 1'024;

    // How often the timer trims a connection's free chunks (see chunk_pool::trim()): those none took for as long are
    // freed, so steady traffic (e.g. a WebSocket's messages, coming every second or more often) reuses them, and an
    // idle connection holds none within twice this.
    constexpr auto trim_interval = std::chrono::seconds{1};

    // How much output may be waiting for the writer before streams' writes wait for it to catch up.
    constexpr std::size_t max_output = std::size_t{1} << 20;

    // How much DATA may wait for the writer (or be being written) before streams' writes wait in the scheduler for
    // their turns (see step_write()): what the scheduler can't reorder, as it's framed already.
    constexpr std::size_t max_ahead = std::size_t{64} * 1'024;

    // How long a stream whose write was all framed keeps its turn for its writer's next write (see fill()).
    constexpr auto hold_turn = std::chrono::milliseconds{1};

    // How long, and how much, the reader goes on reading once the connection's ending, for the peer to close its side
    // (see run()), as an HTTP/1.1 server lingers after an error response.
    constexpr auto        linger_time      = std::chrono::seconds{2};
    constexpr std::size_t max_linger_bytes = std::size_t{256} * 1'024;

    // connection_specific is whether a field is HTTP/1's connection management, which HTTP/2 mustn't send (RFC 9113,
    // section 8.2.2).
    bool connection_specific(std::string_view name) noexcept
    {
        using namespace std::string_view_literals;
        return util::equal_ignore_case(name, "connection"sv) || util::equal_ignore_case(name, "keep-alive"sv)
            || util::equal_ignore_case(name, "proxy-connection"sv)
            || util::equal_ignore_case(name, "transfer-encoding"sv) || util::equal_ignore_case(name, "upgrade"sv);
    }

    std::optional<io::clock::time_point> after(std::chrono::milliseconds timeout)
    {
        if (timeout <= std::chrono::milliseconds::zero()) return std::nullopt;
        return io::clock::now() + timeout;
    }

}

// NOLINTNEXTLINE(bugprone-exception-escape): substr's positions are in range.
std::string_view trim_value(std::string_view value) noexcept
{
    constexpr std::string_view whitespace = " \t";

    const auto first = value.find_first_not_of(whitespace);
    if (first == std::string_view::npos) return {};
    return value.substr(first, value.find_last_not_of(whitespace) - first + 1);
}

template<std::size_t N>
std::error_code append_fields(const basic_headers<N>&    from,
                              std::vector<std::string>&  names,
                              std::vector<hpack::field>& fields,
                              field_section              section)
{
    using namespace std::string_view_literals;

    // as HTTP/1.1's encoder checks them: a peer may pass them on over HTTP/1.1.
    if (!valid_fields(from)) return std::make_error_code(std::errc::invalid_argument);

    // NOTE: reserved, as fields view the names, which mustn't move.
    names.clear();
    names.reserve(from.size());

    // the fields Connection names are this connection's too (RFC 9113, section 8.2.2).
    const bool has_connection = from.get_all("Connection"sv).has_value();

    const bool request = section == field_section::request;
    for (const auto& [name, values] : from)
    {
        // NOTE: but a request's TE, which HTTP/1.1 lists in Connection (RFC 9110, section 10.1.4), and HTTP/2 allows
        //       (as trailers), whether or not Connection names it.
        if (connection_specific(name)) continue;
        if (has_connection && from.has_token("Connection"sv, name)
            && !(request && util::equal_ignore_case(std::string_view{name}, "te"sv)))
            continue;
        if (request && util::equal_ignore_case(std::string_view{name}, "host"sv)) continue;
        if (section == field_section::trailers && !trailer_allowed(name)) continue;

        auto& lowered = names.emplace_back(name);
        std::ranges::transform(lowered, lowered.begin(), [](char c) { return util::to_lower_ascii(c); });

        for (const auto& value : values)
        {
            // NOTE: a view of the value, so trimming it copies nothing.
            const auto trimmed = trim_value(value);
            if (request && lowered == "te" && trimmed != "trailers") continue;
            fields.push_back({.name = lowered, .value = trimmed, .never_indexed = hpack::sensitive(lowered)});
        }
    }

    return {};
}

template std::error_code append_fields(const headers&             from,
                                       std::vector<std::string>&  names,
                                       std::vector<hpack::field>& fields,
                                       field_section              section);
template std::error_code append_fields(const trailer_fields&      from,
                                       std::vector<std::string>&  names,
                                       std::vector<hpack::field>& fields,
                                       field_section              section);

chunk_pool::~chunk_pool()
{
    while (first != nullptr) delete std::exchange(first, first->next);
}

received_chunk* chunk_pool::take()
{
    // NOTE: new, not make_unique (nor new received_chunk{}), which would zero the bytes, that are written before
    //       they're read.
    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory): the chunk's owned by its list (see received_data).
    if (first == nullptr) return new received_chunk;

    auto* chunk = std::exchange(first, first->next);
    chunk->next = nullptr;
    --count;
    fewest = std::min(fewest, count);
    return chunk;
}

void chunk_pool::give(received_chunk* chunk) noexcept
{
    if (count >= limit)
    {
        delete chunk; // NOLINT(cppcoreguidelines-owning-memory): the pool's own, given back
        return;
    }

    chunk->next = std::exchange(first, chunk);
    ++count;
}

void chunk_pool::set_limit(std::size_t n) noexcept
{
    limit = n;
    for (; count > limit; --count) delete std::exchange(first, first->next); // NOLINT(cppcoreguidelines-owning-memory)
    fewest = std::min(fewest, count);
}

bool chunk_pool::trim() noexcept
{
    for (; fewest > 0; --fewest, --count)
        delete std::exchange(first, first->next); // NOLINT(cppcoreguidelines-owning-memory)
    fewest = count;
    return count > 0;
}

received_data::~received_data()
{
    while (head != nullptr) delete std::exchange(head, head->next); // NOLINT(cppcoreguidelines-owning-memory)
}

void received_data::append(std::span<const std::byte> data, chunk_pool& pool)
{
    while (!data.empty())
    {
        if (tail == nullptr || end == received_chunk::capacity)
        {
            auto* chunk = pool.take();
            if (tail == nullptr) head = chunk;
            else tail->next = chunk;
            tail = chunk;
            end  = 0;
        }

        const auto n = std::min(data.size(), received_chunk::capacity - end);
        std::ranges::copy(data.first(n), tail->bytes.begin() + static_cast<std::ptrdiff_t>(end));
        end += n;
        count += n;
        data = data.subspan(n);
    }
}

std::size_t received_data::take(std::span<std::byte> into, chunk_pool& pool) noexcept
{
    std::size_t taken = 0;
    while (taken < into.size() && count > 0)
    {
        // the head's written up to end if it's the tail, otherwise to the end of its bytes.
        const auto written = head == tail ? end : received_chunk::capacity;
        const auto n       = std::min(into.size() - taken, written - start);
        std::ranges::copy(std::span{head->bytes}.subspan(start, n), into.begin() + static_cast<std::ptrdiff_t>(taken));
        start += n;
        taken += n;
        count -= n;

        // read to its end: back to the pool, as the next DATA may well need it (or another stream's).
        if (start == written)
        {
            auto* drained = std::exchange(head, head->next);
            if (head == nullptr)
            {
                tail = nullptr;
                end  = 0;
            }
            start = 0;
            pool.give(drained);
        }
    }
    return taken;
}

void received_data::clear(chunk_pool& pool) noexcept
{
    while (head != nullptr) pool.give(std::exchange(head, head->next));
    tail  = nullptr;
    start = 0;
    end   = 0;
    count = 0;
}

namespace
{

    // take_received takes what a stream's received into data, as much as fits, giving the peer back as much window
    // (holding the lock), and returns how much it took.
    std::size_t take_received(http2_stream_base& s, std::span<std::byte> data)
    {
        // (a tunnel's read that waited has its time: the timer needn't wake it.)
        if (s.tunnel != nullptr) s.tunnel->wake_at.reset();

        const auto n = s.received.take(data, s.conn->chunks);
        s.conn->trim_later();

        // the peer may send as much more.
        s.conn->session.consume(s.id, n);
        return n;
    }

    // tunnel_wait is what a read with nothing to read comes to before it waits (holding the lock): for a tunnel, see
    // stream_tunnel::check_wait(); otherwise, nothing.
    std::error_code tunnel_wait(http2_stream_base& s, std::optional<io::clock::time_point>& timeout_at)
    {
        return s.tunnel != nullptr ? s.tunnel->check_wait(timeout_at) : std::error_code{};
    }

}

coro::task<io::result> stream_reader::read(std::span<std::byte> data)
{
    if (at_start && !data.empty()) [[unlikely]]
        return read_after_start(data);

    // usually, a stream that's being read has received something by now (e.g. a tunnel's reads of a large message, a
    // piece at a time): then there's no need for a coroutine (see coro::ready_task). Otherwise the read waits, as an
    // operation, which looks again first.
    auto&       c      = *stream->conn;
    std::size_t n      = 0;
    bool        output = false;
    {
        std::scoped_lock lock{c.mu};
        auto&            s = *stream;

        if (s.received.empty())
        {
            if (!waiting.claim(data))
                return coro::ready_task<io::result>(
                    io::result{.err = std::make_error_code(std::errc::device_or_resource_busy)});
            return coro::operation_task(waiting);
        }

        n      = take_received(s, data);
        output = c.session.has_output();
    }

    // a WINDOW_UPDATE goes now: the peer may be waiting for it, while this reader goes on reading what's buffered (or
    // stops reading for a while).
    if (output) c.notify_writer();
    return coro::ready_task<io::result>(io::result{.count = n});
}

coro::task<io::result> stream_reader::read_after_start(std::span<std::byte> data)
{
    const auto callback = std::exchange(at_start, {});
    if (auto err = co_await callback.call(callback.context); err) co_return io::result{.err = err};
    co_return co_await read(data);
}

bool stream_read::claim(std::span<std::byte> into) noexcept
{
    if (!try_claim()) return false;

    data   = into;
    filled = 0;
    err    = {};
    woken  = false;
    busy   = false;
    timeout_at.reset();
    return true;
}

bool stream_read::ready()
{
    auto& s = *stream;
    if (filled > 0 || !s.received.empty()) return true;

    if (s.received_end) err = make_error_code(io::errc::end_of_stream);
    else if (s.failure) err = s.failure;
    // a tunnel's read that's stopped, or out of time, fails having read nothing: the stream goes on.
    else err = tunnel_wait(s, timeout_at);
    return static_cast<bool>(err);
}

bool stream_read::start(std::coroutine_handle<> caller) noexcept
{
    auto&            c = *stream->conn;
    std::scoped_lock lock{c.mu};
    auto&            s = *stream;

    // (another reader's: e.g. a request's body's, once its stream's a tunnel.)
    busy = s.reading != nullptr;
    if (busy || ready()) return false;

    s.reading = this;
    awaiting  = caller;
    c.arm_deadline(s.read_deadline, s.read_timeout);

    // (what earlier reads gave back goes now, if it hasn't.)
    if (c.session.has_output()) c.notify_writer();
    return true;
}

std::span<const std::byte> stream_read::take(std::span<const std::byte> incoming) noexcept
{
    // finished for something else (e.g. its tunnel's stop): what comes is the next read's.
    if (woken && filled == 0) return incoming;

    const auto n = std::min(incoming.size(), data.size() - filled);
    std::ranges::copy(incoming.first(n), data.begin() + static_cast<std::ptrdiff_t>(filled));
    filled += n;
    return incoming.subspan(n);
}

void stream_read::wake() noexcept
{
    if (woken || !ready()) return;
    woken = true;

    // NOTE: on the pool, never here: this holds the connection's lock, which the reader takes for its result. A pool
    //       refuses only once it's shutting down, which it doesn't while its scheduler's tasks (the connection's) run.
    if (!stream->conn->get_scheduler()->pool()->resume(awaiting))
    {
        stream->conn->get_logger().critical(
            "an HTTP/2 stream's read can't be resumed: the thread pool's shutting down");
        std::terminate();
    }
}

io::result stream_read::result() noexcept
{
    if (busy)
    {
        release();
        return io::result{.err = std::make_error_code(std::errc::device_or_resource_busy)};
    }

    auto&      c = *stream->conn;
    io::result res;
    bool       output = false;
    {
        std::scoped_lock lock{c.mu};
        auto&            s = *stream;
        if (s.reading == this) s.reading = nullptr;

        // (a tunnel's read that waited has its time: the timer needn't wake it.)
        if (s.tunnel != nullptr) s.tunnel->wake_at.reset();

        if (filled > 0)
        {
            // the peer may send as much more.
            c.session.consume(s.id, filled);
            res.count = filled;
        }
        else if (!s.received.empty())
        {
            res.count = take_received(s, data);
        }
        else
        {
            res.err = err;
            if (err == io::errc::end_of_stream) s.hand_over_trailers();
        }
        output = c.session.has_output();
    }

    release();
    if (output) c.notify_writer();
    return res;
}

void stream_read::cancel() noexcept
{
    {
        std::scoped_lock lock{stream->conn->mu};
        if (stream->reading == this) stream->reading = nullptr;
    }
    release();
}

// NOLINTNEXTLINE(readability-make-member-function-const): it finishes the stream's read.
void http2_stream_base::wake_reader() noexcept
{
    if (reading != nullptr) reading->wake();
}

void http2_stream_base::hand_over_trailers() noexcept
{
    if (trailers_to == nullptr || received_trailers == nullptr) return;

    *trailers_to = std::move(*received_trailers);
    received_trailers.reset();
}

io::handle stream_reader::native_handle() const noexcept { return stream->conn->native_handle(); }

namespace
{

    // write_waiting goes on with a stream's write once its first step has to wait (in the scheduler).
    coro::task<io::result> write_waiting(http2_connection_base&             c,
                                         http2_stream_base&                 s,
                                         std::span<const std::byte>         data,
                                         http2_connection_base::write_state state,
                                         http2_connection_base::write_step  step)
    {
        // destroyed while it waits (e.g. a client's send() whose task is destroyed): the writer mustn't touch data,
        // which goes with the caller's frame, after this one.
        util::defer unqueue{[&c, &s, &step] noexcept
                            {
                                if (!step.done) c.unqueue(s);
                            }};

        // (a step that isn't done has something to wait for.)
        while (!step.done && step.wait != nullptr)
        {
            (void)co_await step.wait->wait(step.seen);
            step = c.step_write(s, data, state);
        }
        co_return step.result;
    }

}

coro::task<io::result> stream_writer::write(std::span<const std::byte> data)
{
    auto&                              c = *stream->conn;
    http2_connection_base::write_state state;

    auto step = c.step_write(*stream, data, state);
    if (step.done) return coro::ready_task<io::result>(step.result);
    return write_waiting(c, *stream, data, state, step);
}

http2_connection_base::write_step
http2_connection_base::step_write(http2_stream_base& s, std::span<const std::byte> data, write_state& state)
{
    const auto done = [](std::size_t count, std::error_code err = {})
    {
        return write_step{
            .done   = true,
            .result = io::result{.count = count, .err = err},
            .wait   = nullptr,
            .seen   = 0
        };
    };
    const auto wait_for = [](coro::notifier& n)
    { return write_step{.done = false, .result = {}, .wait = &n, .seen = n.value()}; };

    write_step      step;
    std::error_code failed; // submitting failed
    bool            output   = false;
    bool            canceled = false; // a tunnel's stopped write, which would have to wait
    {
        std::scoped_lock lock{mu};

        if (state.queued)
        {
            // the writer frames the rest: the write's done once it's all framed (the stream's out of the scheduler),
            // or the stream's failed (which took it out).
            if (s.failure) return done(state.written + s.sending_done, s.failure);
            if (!s.queued()) return done(data.size());

            // a tunnel's stopped write goes no further: the writer mustn't touch its span once it's returned.
            if (s.tunnel != nullptr && s.tunnel->stopped)
            {
                send_order.remove(s);
                return done(state.written + s.sending_done, std::make_error_code(std::errc::operation_canceled));
            }
            return wait_for(s.changed);
        }

        if (s.failure) return done(state.written, s.failure);

        // at once while no one waits for a turn (nor for the writer to catch up with control frames and heads); the
        // rest waits its turn (and the window, if that's what it's waiting for).
        if (send_order.empty() && session.output_size() <= max_output) failed = submit_at_once(s, data, state);
        if (!failed && state.written < data.size())
        {
            canceled = s.tunnel != nullptr && s.tunnel->stopped;
            if (!canceled)
            {
                queue_rest(s, data, state);
                step = wait_for(s.changed);
            }
        }

        output = session.has_output();
    }

    if (failed)
    {
        // the handler's token, which its own callbacks may be registered on, so it's after unlocking.
        stop_pending();
        return done(state.written, failed);
    }
    if (canceled)
    {
        if (output) writer_wakeup.notify();
        return done(state.written, std::make_error_code(std::errc::operation_canceled));
    }

    // (a write that's queued has the writer frame it, whether or not there's output yet.)
    if (output || state.queued) writer_wakeup.notify();
    if (state.written == data.size()) return done(state.written);
    return step;
}

std::error_code
http2_connection_base::submit_at_once(http2_stream_base& s, std::span<const std::byte> data, write_state& state)
{
    // a frame at a time (cut as the session cuts them), while less than max_ahead waits for the writer.
    const std::size_t frame = session.peer_settings().max_frame_size;
    while (state.written < data.size() && data_queued + data_writing < max_ahead)
    {
        const auto rest  = data.subspan(state.written);
        const auto chunk = rest.first(std::min(rest.size(), frame));
        const auto sent  = submit_body(s, chunk);
        if (!sent.has_value())
        {
            fail_stream(s, sent.error());
            return sent.error();
        }

        state.written += *sent;
        data_queued += *sent;
        if (*sent > 0) s.write_deadline.reset();
        if (*sent < chunk.size()) break; // no window
    }
    return {};
}

void http2_connection_base::queue_rest(http2_stream_base& s, std::span<const std::byte> data, write_state& state)
{
    s.sending      = data.subspan(state.written);
    s.sending_done = 0;
    send_order.add(s);
    arm_deadline(s.write_deadline, s.write_timeout);
    state.queued = true;
}

std::expected<std::size_t, std::error_code> http2_connection_base::submit_body(http2_stream_base&         s,
                                                                               std::span<const std::byte> data)
{
    // the last of a body of known length ends the stream (if all of it goes: the session ends it only then), rather
    // than an empty DATA frame after it. (Its writer, a limit_writer, never gives more than what's left.)
    const bool last = s.data_left.has_value() && *s.data_left == data.size();
    const auto sent = session.submit_data(s.id, data, last);
    if (!sent.has_value()) return sent;

    if (s.data_left.has_value()) *s.data_left -= std::min(*sent, *s.data_left);
    if (last && *sent == data.size()) s.ended = true;
    return sent;
}

io::handle stream_writer::native_handle() const noexcept { return stream->conn->native_handle(); }

stream_tunnel::stream_tunnel(http2_stream_base& stream, std::stop_token closing)
    : stream{stream}
    , closing_token{std::move(closing)}
    , raw_in{&stream}
    , in{&raw_in, tunnel_buffer_size}
    , out{&stream.writer, tunnel_buffer_size}
{
    stream.conn->session.widen_window(stream.id, tunnel_window);
}

void stream_tunnel::set_read_timeout(std::optional<io::clock::duration> t) noexcept
{
    // (the next wait's: as a socket's, a read already waiting keeps the timeout it began with.)
    std::scoped_lock lock{stream.conn->mu};
    timeout = t;
}

void stream_tunnel::set_read_deadline(std::optional<io::clock::time_point> d) noexcept
{
    // a read that's waiting looks again (see check_wait()), whichever thread this is.
    std::scoped_lock lock{stream.conn->mu};
    deadline = d;
    stream.wake_reader();
}

void stream_tunnel::set_stop_token(std::stop_token stop) noexcept
{
    // NOTE: without the lock, which the callback takes: removing it waits for it, if it's being made elsewhere.
    on_stop.reset();
    {
        std::scoped_lock lock{stream.conn->mu};
        stopped = false;
    }

    // (made at once, if stop's been requested already.)
    if (stop.stop_possible()) on_stop.emplace(std::move(stop), stopper{this});
}

void stream_tunnel::stopper::operator()() const noexcept
{
    // NOTE: the flag's guarded by the connection's lock, which the waits check it under, so it needs no ordering of its
    //       own; and this runs on whichever thread stops the token, never holding the lock (see fail_stream()).
    auto& s = tunnel->stream;
    {
        std::scoped_lock lock{s.conn->mu};
        tunnel->stopped = true;
        s.wake_reader();
    }
    s.changed.notify();
}

io::scheduler* stream_tunnel::get_scheduler() const noexcept { return stream.conn->get_scheduler(); }

void stream_tunnel::abort() noexcept
{
    auto& c = *stream.conn;
    {
        std::scoped_lock lock{c.mu};
        if (!stream.failure)
        {
            c.fail_stream(stream, std::make_error_code(std::errc::connection_aborted));
            c.session.reset(stream.id, http2::error_code::cancel);
        }
    }

    stream.changed.notify();
    c.wake_writer();
}

void stream_tunnel::end() noexcept
{
    auto& c = *stream.conn;
    {
        std::scoped_lock lock{c.mu};
        if (stream.failure || stream.ended) return;

        // NOTE: what's been flushed has gone to the session (a write returns once it's all been framed), so this
        //       comes after it.
        stream.ended = true;
        if (auto sent = c.session.submit_data(stream.id, {}, true); !sent.has_value())
            c.fail_stream(stream, sent.error());
    }

    stream.changed.notify();
    c.wake_writer();
}

std::error_code stream_tunnel::check_wait(std::optional<io::clock::time_point>& timeout_at)
{
    wake_at.reset();
    if (stopped) return std::make_error_code(std::errc::operation_canceled);

    const auto now = io::clock::now();
    if (!timeout_at.has_value() && timeout.has_value()) timeout_at = now + *timeout;

    auto until = deadline;
    if (timeout_at.has_value() && (!until.has_value() || *timeout_at < *until)) until = timeout_at;
    if (until.has_value() && *until <= now) return std::make_error_code(std::errc::timed_out);

    wake_at = until;
    if (until.has_value()) stream.conn->wake_timer_at(*until);
    return {};
}

http2_connection_base::http2_connection_base(io::scheduler*               scheduler,
                                             connection&                  conn,
                                             http2::role                  side,
                                             const http2::session_config& config,
                                             spdlog::logger&              logger,
                                             bool                         waits_writable,
                                             writable_function            writable)
    : session{side, config}
    , scheduler{scheduler}
    , pool{scheduler->pool()}
    , conn{conn}
    , logger{logger}
    , waits_writable{waits_writable}
    , writable{std::move(writable)}
    , writer_wakeup{pool}
{}

void http2_connection_base::wake_writer()
{
    bool output = false;
    {
        std::scoped_lock lock{mu};
        output = session.has_output();
    }
    if (output) writer_wakeup.notify();
}

void http2_connection_base::arm_deadline(std::optional<io::clock::time_point>& deadline,
                                         std::chrono::milliseconds             timeout)
{
    if (deadline.has_value()) return;

    deadline = after(timeout);
    if (deadline.has_value() && (!timer_target.has_value() || *deadline < *timer_target)) wake_timer();
}

void http2_connection_base::fail_stream(http2_stream_base& s, std::error_code err)
{
    if (s.failure) return;
    s.failure = err;
    s.wake_reader();

    // the writer mustn't touch the span of a write that's returning.
    send_order.remove(s);

    // a tunnel's user finds its failure in its reads and writes, as a connection's would (e.g. the peer's reset as the
    // transport's end, or a write's timeout), rather than as its handler's token's (which the server's abort() stops
    // still).
    if (s.tunnel != nullptr) return;

    // NOTE: a copy (sharing its state, so not allocating), as the stream may be gone once it's stopped.
    if (s.stop.stop_possible() && !s.stop.stop_requested()) to_stop.push_back(s.stop);
}

void http2_connection_base::wake_timer_at(io::clock::time_point at)
{
    if (!timer_target.has_value() || at < *timer_target) wake_timer();
}

void http2_connection_base::trim_later()
{
    if (trim_at.has_value() || chunks.size() == 0) return;

    trim_at = io::clock::now() + trim_interval;
    wake_timer_at(*trim_at);
}

void http2_connection_base::stop_pending()
{
    std::vector<std::stop_source> sources;
    {
        std::scoped_lock lock{mu};
        sources.swap(to_stop);
    }

    stop_all(sources);
}

void http2_connection_base::stop_all(std::vector<std::stop_source>& sources) const
{
    // the callbacks run here, and are anyone's: e.g. a scheduler wait's, which takes the scheduler's locks, or a
    // handler's, which may take this connection's (e.g. by writing).
    // NOLINTNEXTLINE(cert-dcl03-c,misc-static-assert): not constant: in debug builds, held() reads the lock's owner.
    assert(!mu.held() && "a stream's stop source is stopped holding its connection's lock");

    // NOLINTNEXTLINE(misc-const-correctness): request_stop() isn't const in libc++ (nor in the standard).
    for (auto& source : sources) source.request_stop();
    sources.clear();
}

coro::task<> http2_connection_base::run(std::span<const std::byte> already_read)
{
    (void)workers.spawn(write_loop());
    (void)workers.spawn(timer_loop());

    std::vector<std::byte> pending(already_read.begin(), already_read.end());
    std::vector<std::byte> buffer(read_size);

    auto&           reader   = as_reader(conn);
    bool            read_end = false; // the peer's closed its side (or reading failed)
    std::error_code read_err;
    while (receive(pending))
    {
        auto res = co_await reader.read(buffer);
        pending.insert(pending.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(res.count));
        if (res.err)
        {
            // what was read before the end is still received.
            if (!pending.empty()) (void)receive(pending);
            read_end = true;
            read_err = res.err;
            break;
        }
    }

    // the writer writes what's left (e.g. GOAWAY), then shuts the connection down, so the peer sees it end at once:
    // before the streams are done (e.g. the server's handlers, which may take a while to heed their tokens, or not at
    // all), as after a connection error it MUST be closed (RFC 9113, section 5.4.1), and the peer would wait for the
    // streams GOAWAY says may have been processed until it is.
    {
        std::scoped_lock lock{mu};

        // a TLS alert says why the connection ended (e.g. a server refusing the client's certificate: in TLS 1.3, after
        // the client's handshake is done, so with its first read), which the streams fail with. Otherwise they were
        // cut off.
        if (tls_failure(read_err)) end(read_err);
        else end();
    }
    stop_pending();

    // what the peer sends meanwhile is read, and dropped, until it closes its side too: until then, closing would reset
    // the connection, which may discard what the writer's still sending it (see run()'s declaration). The timer stops
    // the reader at linger_until.
    std::size_t lingered = 0;
    while (!read_end && lingered < max_linger_bytes)
    {
        const auto res = co_await reader.read(buffer);
        lingered += res.count;
        read_end = static_cast<bool>(res.err);
    }

    {
        std::scoped_lock lock{mu};
        reader_done = true;
    }
    wake_timer();
    co_await workers.join();

    // what the timer failed as it stopped, if anything.
    stop_pending();

    // NOTE: the connection itself (its fd, and a server's max_connections slot) is held until the streams are done,
    //       deliberately: that bounds the handlers left running to max_connections * max_concurrent_streams, where
    //       freeing it would let a client reconnect, and pile up more. HTTP/1.1 holds its slot the same way. Their
    //       tokens are stopped (end() failed every stream), so handlers that heed them return soon.
    co_await finish_streams();
}

bool http2_connection_base::receive(std::vector<std::byte>& pending)
{
    bool                          go_on = true;
    bool                          wake  = true;
    std::vector<std::stop_source> stopping; // the streams it failed
    {
        std::scoped_lock lock{mu};
        if (is_ending) return false;

        const auto used = session.receive(pending, *this, io::clock::now());
        if (!used.has_value())
        {
            logger.debug("HTTP/2 connection error: {}", used.error().message());
            end();
            go_on = false;
        }
        else
        {
            pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(*used));
        }

        maybe_end();
        go_on = go_on && !is_ending;

        // NOTE: taken here, rather than with stop_pending() (which would lock again), as it's usually empty.
        stopping.swap(to_stop);

        // the writer only if there's something for it: what the session has to send, or streams waiting their turns,
        // whose windows may have grown. Waking it for nothing would cost a hand-over, and take the place (see
        // coro::thread_pool::resume()) of a stream's reader this readied.
        wake = session.has_output() || !send_order.empty() || is_ending || !go_on;
    }

    if (wake) writer_wakeup.notify();
    stop_all(stopping);
    after_receive();
    return go_on;
}

void http2_connection_base::add_stream(http2_stream_base& s)
{
    active.emplace(s.id, &s);
    chunks.set_limit(chunk_pool::limit_for(active.size()));
}

void http2_connection_base::remove_stream(http2::stream_id id)
{
    const auto it = active.find(id);
    if (it == active.end()) return;

    // what the stream received that no one read is given back, or the connection's window would shrink for good (the
    // session gives back only the connection's, if it's done with the stream).
    auto& s = *it->second;
    if (const auto unread = s.received.size(); unread > 0) session.consume(id, unread);
    s.received.clear(chunks);
    send_order.remove(s);

    active.erase(it);
    chunks.set_limit(chunk_pool::limit_for(active.size()));
    trim_later();
}

void http2_connection_base::on_data(http2::stream_id id, std::span<const std::byte> data, bool end_stream)
{
    const auto it = active.find(id);
    if (it == active.end())
    {
        // no one's reading the stream any more: what it doesn't read, the peer gets back.
        session.consume(id, data.size());
        return;
    }

    // straight into the span of a read that's waiting, if nothing's received before it: what doesn't fit is kept
    // for the next.
    auto& s = *it->second;
    if (s.reading != nullptr && s.received.empty()) data = s.reading->take(data);
    s.received.append(data, chunks);
    if (end_stream) s.received_end = true;
    s.read_deadline.reset();
    s.wake_reader();
}

void http2_connection_base::end_received(http2_stream_base& s)
{
    s.received_end = true;
    s.read_deadline.reset();
    s.wake_reader();
    s.changed.notify();
}

void http2_connection_base::on_stream_reset(http2::stream_id id, std::error_code err)
{
    const auto it = active.find(id);
    if (it == active.end()) return;

    // NO_ERROR asks for nothing more to be sent (RFC 9113, section 8.1). If what the peer sent has ended, it stands
    // (the reader reads to its end before finding the failure); if not, it's cut short, which a code of 0 (no error)
    // can't say.
    if (err == http2::error_code::no_error) err = std::make_error_code(std::errc::connection_reset);

    // a tunnel's reset is the end of its transport, cut short, as a TCP connection's reset is (whatever the code: e.g.
    // a browser's CANCEL when it drops a WebSocket), for what speaks its protocol.
    auto& s = *it->second;
    if (s.tunnel != nullptr) err = std::make_error_code(std::errc::connection_reset);

    fail_stream(s, err);
    s.changed.notify();
}

void http2_connection_base::on_window_update(http2::stream_id id)
{
    // a stream that may send again has made progress, as far as its write deadline's concerned. A write waiting in
    // the scheduler isn't woken (the writer, which receive() wakes, frames it), and its deadline starts again, as it
    // waits still (for its turn).
    const auto grew = [this](http2_stream_base& s)
    {
        if (session.send_window(s.id) > 0)
        {
            s.write_deadline.reset();
            if (s.queued()) arm_deadline(s.write_deadline, s.write_timeout);
        }
        if (!s.queued()) s.changed.notify();
    };

    if (id != 0)
    {
        if (const auto it = active.find(id); it != active.end()) grew(*it->second);
        return;
    }

    for (auto& [_, s] : active) grew(*s);
}

void http2_connection_base::on_settings(const http2::settings& /*peer*/)
{
    // e.g. INITIAL_WINDOW_SIZE, which moves every stream's window.
    for (auto& [_, s] : active) s->changed.notify();
}

coro::task<> http2_connection_base::write_loop()
{
    co_await pool->schedule();

    auto&                         writer = as_writer(conn);
    std::vector<std::byte>        out;
    std::vector<std::stop_source> stopping; // the streams fill() failed
    bool                          wrote = false;
    std::error_code               failed; // the write (or wait) that failed, if one did

    while (true)
    {
        // what the last round wrote has to go (but the mark) before more's framed. A round after one that didn't
        // write needn't wait: the connection was writable after the last that did, and only the peer's changed it.
        if (wrote && waits_writable)
        {
            const auto res = co_await wait_for_socket();
            if (res.err)
            {
                failed = res.err;
                break;
            }
        }

        const auto seen   = writer_wakeup.value();
        const bool ending = take_output(out, std::exchange(wrote, false), stopping);
        stop_all(stopping);

        if (!out.empty())
        {
            // NOTE: a write writes it all (or fails); straight to the writer, rather than through io::write_all(),
            //       a coroutine whose frame each round would allocate, where the socket's write usually needs none.
            const auto res = co_await writer.write(std::span<const std::byte>{out});
            wrote          = true;
            if (res.err)
            {
                failed = res.err;
                break;
            }
            continue;
        }

        // ending, and what was left is written (e.g. GOAWAY).
        if (ending) break;

        // NOTE: end() notifies it.
        (void)co_await writer_wakeup.wait(seen);
    }

    if (failed) co_await write_failed(failed);

    // over TLS, after close_notify.
    (void)co_await shutdown(conn);
}

coro::task<> http2_connection_base::write_failed(std::error_code failed)
{
    logger.debug("HTTP/2 connection's write failed: {}", failed.message());

    // the connection was closed (e.g. reset): the reader finds that too, at once, and why, if the peer said (e.g. a
    // TLS alert it sent before closing, which a write after it doesn't see), so it's left to end it. A TLS failure
    // (e.g. the peer's alert, which the stream's writes and waits return once its read has taken it in) says why
    // too: the connection's ended with it, as the reader ends it, so the streams fail with it whichever comes
    // first. Otherwise (e.g. a write, or a wait for the socket, that timed out), it's ended here as cut off.
    if (failed == io::status_condition::closed)
    {
        // NOTE: end() notifies it.
        while (true)
        {
            const auto ended = writer_wakeup.value();
            {
                std::scoped_lock lock{mu};
                if (is_ending) break;
            }
            (void)co_await writer_wakeup.wait(ended);
        }
    }
    else
    {
        {
            std::scoped_lock lock{mu};
            if (tls_failure(failed)) end(failed);
            else end();
        }
        stop_pending();
    }
}

coro::task<io::result> http2_connection_base::wait_for_socket()
{
    if (writable) return writable();
    return wait_writable(conn);
}

bool http2_connection_base::take_output(std::vector<std::byte>&        out,
                                        bool                           wrote,
                                        std::vector<std::stop_source>& stopping)
{
    std::scoped_lock lock{mu};

    // what was written is past: a stream that's waiting its turn meanwhile isn't held up by the peer (see
    // waiting_turn()).
    data_writing = 0;
    if (wrote && !send_order.empty()) wrote_at = io::clock::now();

    // the control frames and heads (in the order they were submitted, as HPACK's encoding is), then DATA in the
    // streams' turns.
    fill();
    session.take_output(out);
    data_writing = std::exchange(data_queued, 0);

    stopping.swap(to_stop);
    return is_ending;
}

coro::task<> http2_connection_base::timer_loop()
{
    co_await pool->schedule();

    while (true)
    {
        std::optional<io::clock::time_point> next;
        std::uint64_t                        seen         = 0;
        bool                                 stop_reading = false;
        std::vector<std::stop_source>        stopping; // the streams it failed
        {
            std::scoped_lock lock{mu};
            if (reader_done) break;

            seen           = timer_word.load(std::memory_order::acquire);
            const auto now = io::clock::now();
            if (!is_ending)
            {
                next           = check_streams(now);
                const auto own = check_connection(now);
                if (own.has_value() && (!next.has_value() || *own < *next)) next = own;

                // a held turn that's over is let go here, not left to the writer's fill(): the writer may be away
                // for long (e.g. waiting for the socket), and its deadline, past, would end each wait at once, so
                // the timer would spin until it's back. (The writer's woken below, each time, to frame the others.)
                if (turn_held != 0 && turn_held_until <= now) turn_held = 0;
                if (turn_held != 0 && (!next.has_value() || turn_held_until < *next)) next = turn_held_until;

                trim_chunks(now, next);
            }

            // (perhaps ended just now:) the reader lingers until linger_until (see run()).
            if (is_ending)
            {
                stop_reading = linger_until <= now;
                next         = linger_until;
            }

            timer_target = next;
            stopping.swap(to_stop);
        }

        stop_all(stopping);

        if (stop_reading)
        {
            // the peer hasn't closed its side: the reader finds the end of the stream.
            socket_of(conn).shutdown_read();
            break;
        }

        writer_wakeup.notify();

        // stopping the scheduler ends the wait (and the connection, as its reads fail).
        const auto err = co_await scheduler->wait_on(timer_word, seen, next);
        if (err == std::errc::operation_canceled) break;
    }
}

void http2_connection_base::trim_chunks(io::clock::time_point now, std::optional<io::clock::time_point>& next)
{
    if (!trim_at.has_value()) return;

    // the next trim's due while some are kept.
    if (*trim_at <= now)
    {
        if (chunks.trim()) trim_at = now + trim_interval;
        else trim_at.reset();
    }
    if (trim_at.has_value() && (!next.has_value() || *trim_at < *next)) next = trim_at;
}

std::optional<io::clock::time_point> http2_connection_base::check_streams(io::clock::time_point now)
{
    // a stream waiting too long for what it's to receive, or to send, is reset alone.
    std::optional<io::clock::time_point> next;
    for (auto& [id, s] : active)
    {
        const auto expired = [&](const std::optional<io::clock::time_point>& deadline)
        {
            if (!deadline.has_value()) return false;
            if (*deadline <= now) return true;
            if (!next.has_value() || *deadline < *next) next = deadline;
            return false;
        };

        // a tunnel's read that's waiting is woken once its time's up, and fails then, the stream going on.
        if (s->tunnel != nullptr && expired(s->tunnel->wake_at))
        {
            s->tunnel->wake_at.reset();
            s->wake_reader();
        }

        // a write that's waiting its turn, while the writer writes others', isn't held up by the peer: its deadline's
        // put off to its timeout from when the writer last wrote.
        if (s->write_deadline.has_value() && *s->write_deadline <= now && waiting_turn(*s, now))
            s->write_deadline = wrote_at.value_or(now) + s->write_timeout;

        // NOTE: both checked, so the one that hasn't expired still counts for when to check next.
        const bool read_expired  = expired(s->read_deadline);
        const bool write_expired = expired(s->write_deadline);
        if (!read_expired && !write_expired) continue;

        s->read_deadline.reset();
        s->write_deadline.reset();
        if (s->failure) continue; // e.g. reset by the peer, which its waits haven't seen yet

        logger.debug("HTTP/2 stream {} timed out", id);
        fail_stream(*s, std::make_error_code(std::errc::timed_out));
        session.reset(id, http2::error_code::cancel);
        s->changed.notify();
    }
    return next;
}

bool http2_connection_base::waiting_turn(const http2_stream_base& s, io::clock::time_point now) const
{
    return s.queued() && session.send_window(s.id) > 0 && wrote_at.has_value() && *wrote_at + s.write_timeout > now;
}

void http2_connection_base::fill()
{
    // (none's ready without the connection's window: not looked through.)
    if (send_order.empty() || session.send_window(0) <= 0) return;

    // the stream that ended the last round keeps its turn (see below) until it writes again, ends or fails, or for
    // hold_turn: until then, nothing else is framed (the timer lets it go and wakes the writer once it's over).
    if (turn_still_held()) return;

    const std::size_t frame = session.peer_settings().max_frame_size;
    const auto        ready = [this](const scheduler_node& n) { return session.send_window(n.stream()) > 0; };
    while (data_queued + data_writing < max_ahead)
    {
        auto* n = send_order.next(ready);
        if (n == nullptr) break;

        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the scheduler's nodes are streams.
        auto&      s     = static_cast<http2_stream_base&>(*n);
        const auto rest  = s.sending.subspan(s.sending_done);
        const auto chunk = rest.first(std::min(rest.size(), frame));
        const auto sent  = submit_body(s, chunk);
        if (!sent.has_value())
        {
            // (it's taken out of the scheduler.)
            fail_stream(s, sent.error());
            s.changed.notify();
            continue;
        }

        if (*sent == 0) break; // (ready() said it had a window: never, but it mustn't spin)

        // progress: the write's done, or its deadline starts again for the rest.
        s.sending_done += *sent;
        data_queued += *sent;
        s.write_deadline.reset();
        if (s.sending_done == s.sending.size())
        {
            send_order.remove(s);
            s.changed.notify();

            // its writer's next write (a handler's, or a request body's) keeps its turn, rather than the next stream
            // taking it (a less urgent one, or the next of a non-incremental one's urgency; but not an incremental one
            // sharing its urgency, which would anyway): its writer usually writes again within microseconds, but the
            // connection's, if the socket takes what it's framed at once, would be back sooner. So the round ends
            // here, and the turn's held, for hold_turn at most, so a writer that's slower (e.g. reading what it sends
            // from elsewhere) doesn't hold the connection up for long. (Without it, a handler writing a frame at a time
            // got about half the connection when it was more urgent than all the others.) A stream whose last DATA
            // this was has nothing more to write.
            if (!s.ended && hold_turn_of(s)) break;
        }
        else
        {
            arm_deadline(s.write_deadline, s.write_timeout);
        }
    }
}

bool http2_connection_base::hold_turn_of(const http2_stream_base& s)
{
    const auto p = s.current_priority();
    if (send_order.empty() || (p.incremental && send_order.has_urgency(p.urgency))) return false;

    turn_held       = s.id;
    turn_held_until = io::clock::now() + hold_turn;
    if (!timer_target.has_value() || turn_held_until < *timer_target) wake_timer();
    return true;
}

bool http2_connection_base::turn_still_held()
{
    if (turn_held == 0) return false;

    const auto it   = active.find(turn_held);
    const bool done = it == active.end() || it->second->queued() || it->second->ended || it->second->failure;
    if (!done && io::clock::now() < turn_held_until) return true;

    turn_held = 0;
    return false;
}

void http2_connection_base::unqueue(http2_stream_base& s)
{
    std::scoped_lock lock{mu};
    send_order.remove(s);
}

coro::task<bool> http2_connection_base::wait_queued(std::size_t n)
{
    while (true)
    {
        const auto seen = writer_wakeup.value();
        {
            std::scoped_lock lock{mu};
            if (is_ending) co_return false;
            if (send_order.size() >= n) co_return true;
        }

        // (a write that queues wakes the writer.)
        (void)co_await writer_wakeup.wait(seen);
    }
}

void http2_connection_base::end(std::error_code why)
{
    if (is_ending) return;
    is_ending = true;

    for (auto& [_, s] : active)
    {
        fail_stream(*s, why);
        s->changed.notify();
    }

    // the reader stops receiving, and lingers, as long as the timer lets it; the writer writes what's left, and
    // finishes (see run()).
    // NOTE: not shutting down the connection's reading side, which (on macOS) resets the connection if the peer then
    //       sends anything, as it may well, reading what's still to be written.
    linger_until = io::clock::now() + linger_time;
    writer_wakeup.notify();
    wake_timer();
}

void http2_connection_base::wake_timer()
{
    timer_word.fetch_add(1, std::memory_order::acq_rel);
    scheduler->notify_all(timer_word);
}

}
