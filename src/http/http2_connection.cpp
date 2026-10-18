#include "http/http2_connection.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <mutex>
#include <optional>
#include <span>
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

void append_fields(const headers&             from,
                   std::vector<std::string>&  names,
                   std::vector<hpack::field>& fields,
                   bool                       request)
{
    using namespace std::string_view_literals;

    // NOTE: reserved, as fields view the names, which mustn't move.
    names.clear();
    names.reserve(from.size());

    for (const auto& [name, values] : from)
    {
        if (connection_specific(name)) continue;
        if (request && util::equal_ignore_case(std::string_view{name}, "host"sv)) continue;

        auto& lowered = names.emplace_back(name);
        std::ranges::transform(lowered, lowered.begin(), [](char c) { return util::to_lower_ascii(c); });

        for (const auto& value : values)
        {
            if (request && lowered == "te" && value != "trailers") continue;
            fields.push_back({.name = lowered, .value = value, .never_indexed = hpack::sensitive(lowered)});
        }
    }
}

coro::task<io::result> stream_reader::read(std::span<std::byte> data)
{
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
                s.deadline.reset();
                output = c.session.has_output();
            }
            else
            {
                if (s.received_end) co_return io::result{.err = make_error_code(io::errc::end_of_stream)};
                if (s.failure) co_return io::result{.err = s.failure};

                seen = s.changed.value();
                c.set_deadline(s, s.read_timeout);
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

io::handle stream_reader::native_handle() const noexcept { return stream->conn->native_handle(); }

coro::task<io::result> stream_writer::write(std::span<const std::byte> data)
{
    auto&       c       = *stream->conn;
    std::size_t written = 0;

    while (true)
    {
        bool          wait_for_writer = false;
        std::uint64_t seen            = 0;
        {
            std::scoped_lock lock{c.mu};
            auto&            s = *stream;

            if (s.failure) co_return io::result{.count = written, .err = s.failure};

            if (c.session.output_size() > max_output)
            {
                // the writer's behind (the peer reads slowly): wait for it, rather than queueing more.
                wait_for_writer = true;
                seen            = c.output_drained().value();
            }
            else
            {
                const auto sent = c.session.submit_data(s.id, data.subspan(written), false);
                if (!sent.has_value())
                {
                    s.failure = sent.error();
                    co_return io::result{.count = written, .err = s.failure};
                }

                written += *sent;
                if (written == data.size())
                {
                    s.deadline.reset();
                }
                else
                {
                    // no window: wait for the peer to give some back.
                    seen = s.changed.value();
                    c.set_deadline(s, s.write_timeout);
                }
            }
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

void http2_connection_base::set_deadline(http2_stream_base& s, std::chrono::milliseconds timeout)
{
    s.deadline = after(timeout);
    if (s.deadline.has_value() && (!timer_target.has_value() || *s.deadline < *timer_target)) wake_timer();
}

coro::task<> http2_connection_base::run(std::span<const std::byte> already_read)
{
    (void)workers.spawn(write_loop());
    (void)workers.spawn(timer_loop());

    std::vector<std::byte> pending(already_read.begin(), already_read.end());
    std::vector<std::byte> buffer(read_size);

    auto& reader = as_reader(conn);
    while (receive(pending))
    {
        auto res = co_await reader.read(buffer);
        pending.insert(pending.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(res.count));
        if (res.err)
        {
            // what was read before the end is still received.
            if (!pending.empty()) (void)receive(pending);
            break;
        }
    }

    {
        std::scoped_lock lock{mu};
        end();
    }

    co_await finish_streams();

    // the writer writes what's left (e.g. GOAWAY), then finishes; and so does the timer.
    writer_wakeup.close();
    wake_timer();
    co_await workers.join();
}

bool http2_connection_base::receive(std::vector<std::byte>& pending)
{
    bool go_on = true;
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
    }

    writer_wakeup.notify();
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
    s.changed.notify();
}

void http2_connection_base::on_stream_reset(http2::stream_id id, std::error_code err)
{
    const auto it = active.find(id);
    if (it == active.end()) return;

    if (!it->second->failure) it->second->failure = err;
    it->second->changed.notify();
}

void http2_connection_base::on_window_update(http2::stream_id id)
{
    if (id != 0)
    {
        if (const auto it = active.find(id); it != active.end()) it->second->changed.notify();
        return;
    }

    for (auto& [_, s] : active) s->changed.notify();
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
        const auto seen = writer_wakeup.value();
        {
            std::scoped_lock lock{mu};
            session.take_output(out);
        }

        if (!out.empty())
        {
            const auto res = co_await io::write_all(writer, out);
            drained.notify();
            if (res.err)
            {
                logger.debug("HTTP/2 connection's write failed: {}", res.err.message());
                std::scoped_lock lock{mu};
                end();
                break;
            }
            continue;
        }

        if (!co_await writer_wakeup.wait(seen))
        {
            // closed: what's left is written (e.g. GOAWAY), and then the writer's done.
            {
                std::scoped_lock lock{mu};
                session.take_output(out);
            }
            if (!out.empty()) (void)co_await io::write_all(writer, out);
            break;
        }
    }

    drained.close();
}

coro::task<> http2_connection_base::timer_loop()
{
    co_await pool->schedule();

    while (true)
    {
        std::optional<io::clock::time_point> next;
        std::uint64_t                        seen = 0;
        {
            std::scoped_lock lock{mu};
            if (is_ending) break;

            seen           = timer_word.load(std::memory_order::acquire);
            const auto now = io::clock::now();
            next           = check_streams(now);
            const auto own = is_ending ? std::nullopt : check_connection(now);
            if (own.has_value() && (!next.has_value() || *own < *next)) next = own;

            timer_target = next;
            if (is_ending) break;
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
        if (!s->deadline.has_value()) continue;
        if (*s->deadline > now)
        {
            if (!next.has_value() || *s->deadline < *next) next = s->deadline;
            continue;
        }

        logger.debug("HTTP/2 stream {} timed out", id);
        s->deadline.reset();
        s->failure = std::make_error_code(std::errc::timed_out);
        session.reset(id, http2::error_code::cancel);
        s->changed.notify();
    }

    return next;
}

void http2_connection_base::end()
{
    if (is_ending) return;
    is_ending = true;

    for (auto& [_, s] : active)
    {
        if (!s->failure) s->failure = std::make_error_code(std::errc::connection_aborted);
        s->changed.notify();
    }

    // the reader stops (finding the end of the stream), the writer writes what's left, and the timer stops.
    socket_of(conn).shutdown_read();
    writer_wakeup.notify();
    wake_timer();
}

void http2_connection_base::wake_timer()
{
    timer_word.fetch_add(1, std::memory_order::acq_rel);
    scheduler->notify_all(timer_word);
}

}
