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
#include "http/http2_session.hpp"
#include "io/io.hpp"
#include "io/reader.hpp"
#include "io/scheduler.hpp"
#include "tcp.hpp"
#include "util/string_util.hpp"

namespace net::http::detail
{

namespace
{

    // How much is read from the connection at once.
    constexpr std::size_t read_size = std::size_t{32} * 1'024;

    // How much output may be waiting for the writer before streams' writes wait for it to catch up.
    constexpr std::size_t max_output = std::size_t{1} << 20;

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

std::error_code append_fields(const headers&             from,
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
        if (connection_specific(name) || (has_connection && from.has_token("Connection"sv, name))) continue;
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

coro::task<io::result> stream_writer::write(std::span<const std::byte> data)
{
    auto&       c       = *stream->conn;
    std::size_t written = 0;

    while (true)
    {
        bool            wait_for_writer = false;
        std::uint64_t   seen            = 0;
        std::error_code failed; // submitting failed
        {
            std::scoped_lock lock{c.mu};
            auto&            s = *stream;

            if (s.failure) co_return io::result{.count = written, .err = s.failure};

            if (c.session.output_size() > max_output)
            {
                // the writer's behind (the peer reads slowly): wait for it, rather than queueing more; for as long as
                // the stream may go without progress.
                wait_for_writer = true;
                seen            = c.output_drained().value();
                c.arm_deadline(s.write_deadline, s.write_timeout);
            }
            else
            {
                const auto sent = c.session.submit_data(s.id, data.subspan(written), false);
                if (!sent.has_value())
                {
                    c.fail_stream(s, sent.error());
                    failed = sent.error();
                }
                else
                {
                    written += *sent;
                    if (*sent > 0) s.write_deadline.reset();

                    if (written < data.size())
                    {
                        // no window: wait for the peer to give some back.
                        seen = s.changed.value();
                        c.arm_deadline(s.write_deadline, s.write_timeout);
                    }
                }
            }
        }

        if (failed)
        {
            // the handler's token, which its own callbacks may be registered on, so it's after unlocking.
            c.stop_pending();
            co_return io::result{.count = written, .err = failed};
        }

        c.wake_writer();
        if (written == data.size()) co_return io::result{.count = written};

        if (wait_for_writer) (void)co_await c.output_drained().wait(seen);
        else (void)co_await stream->changed.wait(seen);
    }
}

io::handle stream_writer::native_handle() const noexcept { return stream->conn->native_handle(); }

http2_connection_base::http2_connection_base(io::scheduler*               scheduler,
                                             connection&                  conn,
                                             http2::role                  side,
                                             const http2::session_config& config,
                                             spdlog::logger&              logger)
    : session{side, config}
    , scheduler{scheduler}
    , pool{scheduler->pool()}
    , conn{conn}
    , logger{logger}
    , writer_wakeup{pool}
    , drained{pool}
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

    // a writer may be waiting for the connection's writer to catch up, rather than for the stream.
    drained.notify();
}

void http2_connection_base::on_window_update(http2::stream_id id)
{
    // a stream that may send again has made progress, as far as its write deadline's concerned.
    const auto grew = [this](http2_stream_base& s)
    {
        if (session.send_window(s.id) > 0) s.write_deadline.reset();
        s.changed.notify();
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

    auto&                  writer = as_writer(conn);
    std::vector<std::byte> out;

    while (true)
    {
        const auto seen   = writer_wakeup.value();
        bool       ending = false;
        {
            std::scoped_lock lock{mu};
            session.take_output(out);
            ending = is_ending;
        }

        if (!out.empty())
        {
            const auto res = co_await io::write_all(writer, out);
            drained.notify();
            if (res.err)
            {
                logger.debug("HTTP/2 connection's write failed: {}", res.err.message());

                // the connection was closed (e.g. reset): the reader finds that too, at once, and why, if the peer said
                // (e.g. a TLS alert it sent before closing, which a write after it doesn't see), so it's left to end
                // it. Otherwise (e.g. a write that timed out), it's ended here.
                if (res.err == io::status_condition::closed)
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
                    break;
                }

                {
                    std::scoped_lock lock{mu};
                    end();
                }
                stop_pending();
                break;
            }
            continue;
        }

        // ending, and what was left is written (e.g. GOAWAY).
        if (ending) break;

        // NOTE: end() notifies it.
        (void)co_await writer_wakeup.wait(seen);
    }

    drained.close();

    // over TLS, after close_notify.
    (void)co_await shutdown(conn);
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
    bool                                 timed_out = false;
    for (auto& [id, s] : active)
    {
        const auto expired = [&](const std::optional<io::clock::time_point>& deadline)
        {
            if (!deadline.has_value()) return false;
            if (*deadline <= now) return true;
            if (!next.has_value() || *deadline < *next) next = deadline;
            return false;
        };

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
        timed_out = true;
    }

    // a writer may be waiting for the connection's writer to catch up, rather than for the stream.
    if (timed_out) drained.notify();
    return next;
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
    drained.notify();

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
