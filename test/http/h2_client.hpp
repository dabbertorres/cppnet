#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "coro/task.hpp"
#include "http/hpack.hpp"
#include "http/http2.hpp"
#include "http/http2_session.hpp"
#include "io/reader.hpp"
#include "io/scheduler.hpp"
#include "io/util.hpp"
#include "io/writer.hpp"
#include "socket.hpp"

namespace test
{

namespace http2 = net::http::http2;
namespace hpack = net::http::hpack;

// h2_client is a minimal HTTP/2 client for the tests: a session over a connection, with what it has received. With
// role::server, it's a minimal server instead (for the client's tests), whose responses are the requests it receives.
struct h2_client final : http2::session_events
{
    struct response
    {
        std::string                                      status;
        std::vector<std::pair<std::string, std::string>> headers;
        std::string                                      body;
        bool                                             done = false;
        std::error_code                                  reset;
        int                                              order = 0; // when it was done
    };

    // How long pump() waits by default, so a server that stops answering fails a test rather than hanging it.
    static constexpr std::chrono::seconds default_timeout{10};

    // sock is the connection's socket, for pump()'s deadlines, and to close its side once the server has (none if
    // it's null).
    h2_client(net::io::reader*             in,
              net::io::writer*             out,
              net::socket*                 sock   = nullptr,
              http2::role                  side   = http2::role::client,
              const http2::session_config& config = {})
        : s{side, config}
        , in{in}
        , out{out}
        , sock{sock}
    {}

    http2::session                       s;
    net::io::reader*                     in;
    net::io::writer*                     out;
    net::socket*                         sock;
    std::map<http2::stream_id, response> responses;
    bool                                 goaway = false;
    std::error_code                      goaway_error;
    http2::stream_id                     goaway_last = 0; // the last GOAWAY's last stream
    bool                                 closed      = false;
    bool                                 timed_out   = false; // the last pump() reached its deadline
    int                                  finished    = 0;
    int                                  pings       = 0; // acknowledged

    void
    on_headers(http2::stream_id id, http2::block_kind kind, std::span<const hpack::field> fields, bool end) override
    {
        auto& r = responses[id];
        if (kind == http2::block_kind::head)
        {
            for (const auto& f : fields)
            {
                if (f.name == ":status") r.status = f.value;
                else r.headers.emplace_back(f.name, f.value);
            }
        }
        if (end) done(r);
    }

    void on_data(http2::stream_id id, std::span<const std::byte> data, bool end) override
    {
        auto& r = responses[id];
        r.body.append(reinterpret_cast<const char*>(data.data()), data.size());
        s.consume(id, data.size());
        if (end) done(r);
    }

    void on_stream_reset(http2::stream_id id, std::error_code err) override
    {
        auto& r = responses[id];
        r.reset = err;
        done(r);
    }

    void on_window_update(http2::stream_id /*stream*/) override {}
    void on_settings(const http2::settings& /*peer*/) override {}
    void on_goaway(http2::stream_id last, std::error_code err, std::string_view /*debug*/) override
    {
        goaway       = true;
        goaway_error = err;
        goaway_last  = last;
    }
    void on_ping_ack(std::span<const std::byte, 8> /*opaque*/) override { ++pings; }

    void done(response& r)
    {
        r.done  = true;
        r.order = ++finished;
    }

    // request opens a stream, and sends a request's head (with fields, if any) and body (if any).
    http2::stream_id request(std::string_view              method,
                             std::string_view              path,
                             std::string_view              body   = {},
                             bool                          end    = true,
                             std::span<const hpack::field> fields = {})
    {
        const auto id = s.open_stream().value();

        std::vector<hpack::field> head{
            {   .name = ":method",      .value = method},
            {   .name = ":scheme",      .value = "http"},
            {.name = ":authority", .value = "localhost"},
            {     .name = ":path",        .value = path}
        };
        head.insert(head.end(), fields.begin(), fields.end());
        REQUIRE_FALSE(s.submit_headers(id, head, body.empty() && end));
        if (!body.empty()) send(id, body, end);
        return id;
    }

    // respond sends a response's head (as a server), and its body, if any (ending the stream, if end).
    void respond(http2::stream_id id, std::string_view status, std::string_view body = {}, bool end = true)
    {
        const std::vector<hpack::field> head{
            {.name = ":status", .value = status}
        };
        REQUIRE_FALSE(s.submit_headers(id, head, body.empty() && end));
        if (!body.empty()) send(id, body, end);
    }

    // send sends as much of a body as the windows allow (pump() sends the rest as they grow).
    void send(http2::stream_id id, std::string_view body, bool end)
    {
        pending_body[id] = std::string{body};
        pending_end[id]  = end;
        send_pending();
    }

    void send_pending()
    {
        for (auto it = pending_body.begin(); it != pending_body.end();)
        {
            auto& [id, body] = *it;
            const auto sent  = s.submit_data(id, std::as_bytes(std::span{body}), pending_end[id]);
            if (sent.has_value()) body.erase(0, *sent);
            it = body.empty() || !sent.has_value() ? pending_body.erase(it) : std::next(it);
        }
    }

    // pump writes what's to be written, and reads and receives, until done() says so, the connection ends, or (if
    // the socket's known) timeout passes, returning done().
    template<typename Done>
    net::coro::task<bool> pump(Done done, net::io::clock::duration timeout = default_timeout)
    {
        std::vector<std::byte> buffer(16'384);
        std::vector<std::byte> output;

        timed_out = false;
        if (sock != nullptr) sock->set_read_deadline(net::io::clock::now() + timeout);

        while (true)
        {
            send_pending();
            s.take_output(output);
            if (!output.empty() && (co_await net::io::write_all(*out, output)).err)
            {
                closed = true;
                co_return done();
            }

            if (done()) co_return true;
            if (closed) co_return false;

            auto res = co_await in->read(buffer);
            pending.insert(pending.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(res.count));
            if (res.err == std::errc::timed_out)
            {
                // NOTE: what was read before is received, but nothing more is waited for.
                timed_out = true;
            }
            else if (res.err)
            {
                closed = true;

                // as a client would, it closes its side too, so the server needn't linger for it.
                if (sock != nullptr) sock->shutdown_write();
            }

            const auto used = s.receive(pending, *this, std::chrono::steady_clock::now());
            if (!used.has_value())
            {
                closed = true;
                co_return done();
            }
            pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(*used));
            if (timed_out) co_return done();
        }
    }

    net::coro::task<bool> wait_for(http2::stream_id id, net::io::clock::duration timeout = default_timeout)
    {
        return pump([this, id] { return responses[id].done; }, timeout);
    }

    // window_update sends WINDOW_UPDATE (which a session sends only as it consumes data) at once.
    net::coro::task<bool> window_update(http2::stream_id id, std::uint32_t increment) const
    {
        std::vector<std::byte> frame;
        http2::encode_window_update(id, increment, frame);
        co_return !(co_await net::io::write_all(*out, frame)).err;
    }

    // sync sends a PING, and pumps until it's acknowledged: by then, the server has received all that was sent before.
    net::coro::task<bool> sync()
    {
        constexpr std::array<std::byte, 8> opaque{};
        s.ping(opaque);
        return pump([this, want = pings + 1] { return pings >= want; });
    }

    std::map<http2::stream_id, std::string> pending_body;
    std::map<http2::stream_id, bool>        pending_end;

    // what's been read but not received (part of a frame): kept from one pump() to the next.
    std::vector<std::byte> pending;
};

// gate holds handlers until a test opens it: the n-th to pass waits for the n-th open() (or the scheduler to stop).
struct gate
{
    explicit gate(net::io::scheduler* sched)
        : sched{sched}
    {}

    net::coro::task<> pass()
    {
        const auto n = arrived.fetch_add(1) + 1;
        for (auto seen = opened.load(); seen < n; seen = opened.load())
        {
            if (co_await sched->wait_on(opened, seen) == std::errc::operation_canceled) co_return;
        }
    }

    // pass(stop) passes as pass() does, or once stop is requested, returning whether it was.
    net::coro::task<bool> pass(std::stop_token stop)
    {
        const auto n = arrived.fetch_add(1) + 1;
        for (auto seen = opened.load(); seen < n; seen = opened.load())
        {
            if (co_await sched->wait_on(opened, seen, std::nullopt, stop)) co_return stop.stop_requested();
        }
        co_return false;
    }

    void open()
    {
        opened.fetch_add(1);
        sched->notify_all(opened);
    }

    net::io::scheduler*        sched;
    std::atomic<std::uint64_t> arrived{0};
    std::atomic<std::uint64_t> opened{0};
};

// header is a response's field's value, or "(none)".
inline std::string header(const h2_client::response& r, std::string_view name)
{
    for (const auto& [n, v] : r.headers)
    {
        if (n == name) return v;
    }
    return "(none)";
}

}
