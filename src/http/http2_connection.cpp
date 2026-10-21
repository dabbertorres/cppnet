#include "http/http2_connection.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
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

coro::task<io::result> stream_reader::read(std::span<std::byte> data)
{
    if (at_start && !data.empty()) [[unlikely]]
    {
        const auto callback = std::exchange(at_start, {});
        if (auto err = co_await callback.call(callback.context); err) co_return io::result{.err = err};
    }

    auto& c = *stream->conn;
    while (true)
    {
        std::uint64_t seen   = 0;
        bool          took   = false; // what's buffered
        std::size_t   n      = 0;
        bool          output = false;
        {
            std::scoped_lock lock{c.mu};
            auto&            s = *stream;

            if (s.received_start < s.received.size())
            {
                took = true;
                n    = std::min(data.size(), s.received.size() - s.received_start);
                std::ranges::copy(std::span{s.received}.subspan(s.received_start, n), data.begin());
                s.received_start += n;
                if (s.received_start == s.received.size())
                {
                    s.received.clear();
                    s.received_start = 0;
                }
                else if (s.received_start >= s.received.size() / 2)
                {
                    // a reader that never catches up mustn't keep all it's read: what's left moves to the front, so
                    // the buffer holds at most about twice the stream's window.
                    s.received.erase(s.received.begin(),
                                     s.received.begin() + static_cast<std::ptrdiff_t>(s.received_start));
                    s.received_start = 0;
                }

                // the peer may send as much more.
                c.session.consume(s.id, n);
                output = c.session.has_output();
            }
            else
            {
                if (s.received_end)
                {
                    s.hand_over_trailers();
                    co_return io::result{.err = make_error_code(io::errc::end_of_stream)};
                }
                if (s.failure) co_return io::result{.err = s.failure};

                seen = s.changed.value();
                c.arm_deadline(s.read_deadline, s.read_timeout);
            }
        }

        if (took)
        {
            // a WINDOW_UPDATE goes now: the peer may be waiting for it, while this reader goes on reading what's
            // buffered (or stops reading for a while).
            if (output) c.wake_writer();
            co_return io::result{.count = n};
        }

        c.wake_writer();
        (void)co_await stream->changed.wait(seen);
    }
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
    bool            output = false;
    {
        std::scoped_lock lock{mu};

        if (state.queued)
        {
            // the writer frames the rest: the write's done once it's all framed (the stream's out of the scheduler),
            // or the stream's failed (which took it out).
            if (s.failure) return done(state.written + s.sending_done, s.failure);
            if (!s.queued()) return done(data.size());
            return wait_for(s.changed);
        }

        if (s.failure) return done(state.written, s.failure);

        // at once while no one waits for a turn (nor for the writer to catch up with control frames and heads); the
        // rest waits its turn (and the window, if that's what it's waiting for).
        if (send_order.empty() && session.output_size() <= max_output) failed = submit_at_once(s, data, state);
        if (!failed && state.written < data.size())
        {
            queue_rest(s, data, state);
            step = wait_for(s.changed);
        }

        output = session.has_output();
    }

    if (failed)
    {
        // the handler's token, which its own callbacks may be registered on, so it's after unlocking.
        stop_pending();
        return done(state.written, failed);
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

    // the writer mustn't touch the span of a write that's returning.
    send_order.remove(s);

    // NOTE: a copy (sharing its state, so not allocating), as the stream may be gone once it's stopped.
    if (s.stop.stop_possible() && !s.stop.stop_requested()) to_stop.push_back(s.stop);
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
    }

    writer_wakeup.notify();
    stop_all(stopping);
    after_receive();
    return go_on;
}

void http2_connection_base::add_stream(http2_stream_base& s) { active.emplace(s.id, &s); }

void http2_connection_base::remove_stream(http2::stream_id id)
{
    const auto it = active.find(id);
    if (it == active.end()) return;

    // what the stream received that no one read is given back, or the connection's window would shrink for good (the
    // session gives back only the connection's, if it's done with the stream).
    auto& s = *it->second;
    if (const auto unread = s.received.size() - s.received_start; unread > 0) session.consume(id, unread);
    s.received.clear();
    s.received_start = 0;
    send_order.remove(s);

    active.erase(it);
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

    auto& s = *it->second;
    s.received.insert(s.received.end(), data.begin(), data.end());
    if (end_stream) s.received_end = true;
    s.read_deadline.reset();
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

    auto& s = *it->second;
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
            const auto res = co_await io::write_all(writer, out);
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

    if (failed)
    {
        logger.debug("HTTP/2 connection's write failed: {}", failed.message());

        // the connection was closed (e.g. reset): the reader finds that too, at once, and why, if the peer said (e.g. a
        // TLS alert it sent before closing, which a write after it doesn't see), so it's left to end it. Otherwise
        // (e.g. a write, or a wait for the socket, that timed out), it's ended here.
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
                end();
            }
            stop_pending();
        }
    }

    // over TLS, after close_notify.
    (void)co_await shutdown(conn);
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

                // (the writer's woken below, each time.)
                if (turn_held != 0 && (!next.has_value() || turn_held_until < *next)) next = turn_held_until;
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
    // hold_turn: until then, nothing else is framed (the timer wakes the writer once it's over).
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
            // got about half the connection when it was more urgent than all the others: learned in stage 5's tenth
            // part.) A stream whose last DATA this was has nothing more to write.
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
