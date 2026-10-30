#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "coro/async_scope.hpp"
#include "coro/task.hpp"
#include "http/h2_client.hpp"
#include "http/hpack.hpp"
#include "http/http2.hpp"
#include "io/io.hpp"
#include "io/reader.hpp"
#include "io/scheduler.hpp"
#include "io/writer.hpp"
#include "socket.hpp"

namespace test
{

// h2_tunnels drives an h2_client from tasks of its own, so that several of the tests' tasks may each read and write a
// stream at once, as an io::reader and io::writer (e.g. a scripted WebSocket client per stream of an extended CONNECT,
// test/websocket/raw_peer.hpp): a reader task receives what the server sends, and a writer task writes what the session
// has to send. A stream's DATA is consumed (its window given back) only as its reader reads it, so a stream that isn't
// read holds the server's writes as a client that doesn't read would. Every wait is limited (to 10 s), so a server that
// stops answering fails a test rather than hangs it.
//
// It must be finished (finish(), which ends its tasks) before it's destroyed: see with_tunnels().
class h2_tunnels
{
public:
    // How long a stream's read or write waits, at most.
    static constexpr std::chrono::seconds wait_limit{10};

    // sock is the connection's socket (under in and out, if they're TLS's), to end the reader task with.
    h2_tunnels(net::io::scheduler* sched, net::io::reader* in, net::io::writer* out, net::socket* sock)
        : sched{sched}
        , in{in}
        , out{out}
        , sock{sock}
        , c{in, out, sock}
    {
        c.hold_windows = true;
    }

    h2_tunnels(const h2_tunnels&)            = delete;
    h2_tunnels& operator=(const h2_tunnels&) = delete;
    h2_tunnels(h2_tunnels&&)                 = delete;
    h2_tunnels& operator=(h2_tunnels&&)      = delete;
    ~h2_tunnels()                            = default;

    // stream is one of the connection's streams, as a reader and writer of its DATA.
    class stream final
        : public net::io::reader
        , public net::io::writer
    {
    public:
        stream(h2_tunnels* owner, http2::stream_id id) noexcept
            : owner{owner}
            , id{id}
        {}

        // read reads what the stream received, as it comes; its end (END_STREAM) is the end of the stream, and a reset
        // its error (connection_reset for NO_ERROR, which can't say it was cut short).
        net::coro::task<net::io::result> read(std::span<std::byte> data) override
        {
            const auto deadline = net::io::clock::now() + wait_limit;
            while (true)
            {
                const auto seen = owner->changes.load();
                {
                    std::scoped_lock lock{owner->mu};
                    auto&            r    = owner->c.responses[id];
                    auto&            from = owner->taken[id];
                    if (from < r.body.size())
                    {
                        const auto n = std::min(data.size(), r.body.size() - from);
                        std::ranges::copy(std::as_bytes(std::span{r.body}).subspan(from, n), data.begin());
                        from += n;
                        owner->c.s.consume(id, n);
                        owner->wake_writer();
                        co_return net::io::result{.count = n};
                    }
                    if (r.reset) co_return net::io::result{.err = r.reset};
                    if (r.done) co_return net::io::result{.err = make_error_code(net::io::errc::end_of_stream)};
                    if (owner->ended)
                        co_return net::io::result{.err = std::make_error_code(std::errc::connection_aborted)};
                }

                if (auto err = co_await owner->sched->wait_on(owner->changes, seen, deadline))
                    co_return net::io::result{.err = err};
            }
        }

        // write sends data as DATA, as the windows allow (waiting for them to grow).
        net::coro::task<net::io::result> write(std::span<const std::byte> data) override
        {
            const auto  deadline = net::io::clock::now() + wait_limit;
            std::size_t sent     = 0;
            while (sent < data.size())
            {
                auto        seen      = owner->changes.load();
                std::size_t submitted = 0;
                {
                    std::scoped_lock lock{owner->mu};
                    if (owner->ended)
                        co_return net::io::result{.count = sent,
                                                  .err   = std::make_error_code(std::errc::connection_aborted)};

                    auto took = owner->c.s.submit_data(id, data.subspan(sent), false);
                    if (!took.has_value()) co_return net::io::result{.count = sent, .err = took.error()};
                    submitted = *took;
                    sent += submitted;
                    owner->wake_writer();
                }

                // the stream's window has changed (for wait_send_window()): this write's own change isn't one to wait
                // for.
                if (submitted > 0)
                {
                    owner->changed();
                    ++seen;
                }

                if (sent == data.size()) break;
                if (auto err = co_await owner->sched->wait_on(owner->changes, seen, deadline))
                    co_return net::io::result{.count = sent, .err = err};
            }
            co_return net::io::result{.count = sent};
        }

        using net::io::reader::read;
        using net::io::writer::write;

        [[nodiscard]] net::io::handle native_handle() const noexcept override { return owner->sock->native_handle(); }

    private:
        h2_tunnels*      owner;
        http2::stream_id id;
    };

    // start starts the reader and writer tasks (each runs on the calling thread until it first waits).
    void start()
    {
        tasks.spawn(read_loop());
        tasks.spawn(write_loop());
    }

    // open waits for the server's SETTINGS, then opens a stream with an extended CONNECT for path (see
    // h2_client::extended_connect()), returning it, or why it couldn't.
    net::coro::task<std::expected<http2::stream_id, std::error_code>>
    open(std::string_view path, std::string_view protocol, std::vector<hpack::field> fields = {}, bool end = false)
    {
        if (!co_await wait_until([this] { return c.s.has_peer_settings(); }))
            co_return std::unexpected(std::make_error_code(std::errc::timed_out));

        std::scoped_lock lock{mu};
        auto             id = c.extended_connect(path, protocol, fields, end);
        wake_writer();
        co_return id;
    }

    // request opens a stream with an ordinary request (see h2_client::request()), without a body.
    http2::stream_id request(std::string_view method, std::string_view path)
    {
        std::scoped_lock lock{mu};
        const auto       id = c.request(method, path);
        wake_writer();
        return id;
    }

    // response waits for a stream's final head (or its end), returning a copy of what it has received.
    net::coro::task<std::optional<h2_client::response>> response(http2::stream_id id)
    {
        const bool came = co_await wait_until(
            [this, id]
            {
                const auto& r = c.responses[id];
                return !r.status.empty() || r.done || ended;
            });
        if (!came) co_return std::nullopt;

        std::scoped_lock lock{mu};
        co_return c.responses[id];
    }

    // done waits for a stream to end (or be reset), returning a copy of what it has received.
    net::coro::task<std::optional<h2_client::response>> done(http2::stream_id id)
    {
        if (!co_await wait_until([this, id] { return c.responses[id].done || ended; })) co_return std::nullopt;

        std::scoped_lock lock{mu};
        co_return c.responses[id];
    }

    // settings is the server's SETTINGS, once they've come.
    [[nodiscard]] http2::settings settings()
    {
        std::scoped_lock lock{mu};
        return c.s.peer_settings();
    }

    // end ends a stream's side (END_STREAM), as a WebSocket's client closes its connection's.
    void end(http2::stream_id id)
    {
        std::scoped_lock lock{mu};
        if (c.s.submit_data(id, {}, true).has_value()) wake_writer();
    }

    // reset resets a stream: its reads then fail with code, as if the server had.
    void reset(http2::stream_id id, http2::error_code code)
    {
        {
            std::scoped_lock lock{mu};
            c.s.reset(id, code);
            c.responses[id].reset = code;
            c.done(c.responses[id]);
            wake_writer();
        }
        changed();
    }

    // goaway sends GOAWAY (the client opens no more streams).
    void goaway()
    {
        std::scoped_lock lock{mu};
        c.s.goaway(c.s.last_peer_stream(), http2::error_code::no_error);
        wake_writer();
    }

    // wait_send_window waits until this side's window for a stream (not the connection's) is window, e.g. once the
    // server's widened it, or once what's been written has used it up, returning whether it was (within wait_limit).
    net::coro::task<bool> wait_send_window(http2::stream_id id, std::int64_t window)
    {
        return wait_until([this, id, window] { return c.s.send_window(id) == window; });
    }

    // wait_received waits until a stream has received at least n bytes of DATA, read or not (or the connection's
    // ended), returning how much it has.
    net::coro::task<std::size_t> wait_received(http2::stream_id id, std::size_t n)
    {
        (void)co_await wait_until([this, id, n] { return c.responses[id].body.size() >= n || ended; });

        std::scoped_lock lock{mu};
        co_return c.responses[id].body.size();
    }

    // can_open is whether the server's MAX_CONCURRENT_STREAMS lets another stream open now.
    [[nodiscard]] bool can_open()
    {
        std::scoped_lock lock{mu};
        return c.s.can_open_stream();
    }

    // connection_ended is whether the connection has ended (the server closed it, or reading or writing failed).
    [[nodiscard]] bool connection_ended()
    {
        std::scoped_lock lock{mu};
        return ended;
    }

    // goaway_received is whether the server has sent GOAWAY.
    [[nodiscard]] bool goaway_received()
    {
        std::scoped_lock lock{mu};
        return c.goaway;
    }

    // wait_ended waits for the connection to end, returning whether it did (within wait_limit).
    net::coro::task<bool> wait_ended()
    {
        return wait_until([this] { return ended; });
    }

    // finish ends the reader and writer tasks (what the session has to send is written first), and waits for them.
    net::coro::task<> finish()
    {
        {
            std::scoped_lock lock{mu};
            finishing = true;
        }
        wake_writer();
        sock->shutdown_read();

        // NOTE: the scope's, as it says the tasks are done once their frames are gone, after which nothing of this is
        //       touched (a count they bumped, then notified, could let this go before the notifying was done).
        co_await tasks.join();
    }

private:
    friend class stream;

    // wait_until waits until met() (called holding the lock) says so, returning false if it doesn't within wait_limit.
    template<typename Met>
    net::coro::task<bool> wait_until(Met met)
    {
        const auto deadline = net::io::clock::now() + wait_limit;
        while (true)
        {
            const auto seen = changes.load();
            {
                std::scoped_lock lock{mu};
                if (met()) co_return true;
            }
            if (co_await sched->wait_on(changes, seen, deadline)) co_return false;
        }
    }

    void wake_writer()
    {
        output.fetch_add(1);
        sched->notify_all(output);
    }

    void changed()
    {
        changes.fetch_add(1);
        sched->notify_all(changes);
    }

    net::coro::task<> read_loop()
    {
        std::vector<std::byte> buffer(16'384);
        while (true)
        {
            auto res = co_await in->read(buffer);

            bool over = false;
            {
                std::scoped_lock lock{mu};
                c.pending.insert(c.pending.end(),
                                 buffer.begin(),
                                 buffer.begin() + static_cast<std::ptrdiff_t>(res.count));
                if (const auto used = c.s.receive(c.pending, c, std::chrono::steady_clock::now()); used.has_value())
                    c.pending.erase(c.pending.begin(), c.pending.begin() + static_cast<std::ptrdiff_t>(*used));
                else ended = true;

                if (res.err) ended = true;
                over = ended;
            }

            // as a client would, it closes its side too, so the server needn't linger for it.
            if (res.err && res.err != std::errc::timed_out) sock->shutdown_write();

            changed();
            wake_writer();
            if (over) break;
        }
    }

    net::coro::task<> write_loop()
    {
        std::vector<std::byte> output_bytes;
        while (true)
        {
            const auto seen = output.load();
            bool       stop = false;
            {
                std::scoped_lock lock{mu};
                c.s.take_output(output_bytes);
                stop = finishing || ended;
            }

            if (!output_bytes.empty())
            {
                const auto wrote = co_await net::io::write_all(*out, output_bytes);
                output_bytes.clear();
                if (wrote.err)
                {
                    {
                        std::scoped_lock lock{mu};
                        ended = true;
                    }
                    changed();
                    break;
                }
                continue;
            }

            if (stop) break;
            if (co_await sched->wait_on(output, seen) == std::errc::operation_canceled) break;
        }
    }

    net::io::scheduler* sched;
    net::io::reader*    in;
    net::io::writer*    out;
    net::socket*        sock;

    std::mutex                              mu;
    h2_client                               c;
    std::map<http2::stream_id, std::size_t> taken; // of each stream's DATA, what its reader has read
    bool                                    ended     = false;
    bool                                    finishing = false;

    std::atomic<std::uint64_t> changes{0}; // something's been received, or the connection's ended
    std::atomic<std::uint64_t> output{0};  // there may be something to write

    // the reader and writer tasks: last, so it's destroyed first (once finish() has joined it, it has none).
    net::coro::async_scope tasks;
};

// with_tunnels runs test with an h2_tunnels over a connection (in, out and sock, as h2_tunnels takes them), finishing
// it afterwards, also if test throws (e.g. a failed REQUIRE), so its tasks are done before it goes.
template<typename Test>
net::coro::task<>
with_tunnels(net::io::scheduler* sched, net::io::reader* in, net::io::writer* out, net::socket* sock, Test test)
{
    h2_tunnels tunnels{sched, in, out, sock};
    tunnels.start();

    std::exception_ptr failure;
    try
    {
        co_await test(tunnels);
    }
    catch (...)
    {
        failure = std::current_exception();
    }

    co_await tunnels.finish();
    if (failure) std::rethrow_exception(failure);
}

}
