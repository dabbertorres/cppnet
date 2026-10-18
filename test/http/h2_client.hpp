#pragma once

#include <chrono>
#include <cstddef>
#include <iterator>
#include <map>
#include <span>
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
#include "io/util.hpp"
#include "io/writer.hpp"

namespace test
{

namespace http2 = net::http::http2;
namespace hpack = net::http::hpack;

// h2_client is a minimal HTTP/2 client for the tests: a session over a connection, with what it has received.
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

    h2_client(net::io::reader* in, net::io::writer* out)
        : in{in}
        , out{out}
    {}

    http2::session                       s{http2::role::client};
    net::io::reader*                     in;
    net::io::writer*                     out;
    std::map<http2::stream_id, response> responses;
    bool                                 goaway   = false;
    bool                                 closed   = false;
    int                                  finished = 0;

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
    void on_goaway(http2::stream_id /*last*/, std::error_code /*err*/, std::string_view /*debug*/) override
    {
        goaway = true;
    }
    void on_ping_ack(std::span<const std::byte, 8> /*opaque*/) override {}

    void done(response& r)
    {
        r.done  = true;
        r.order = ++finished;
    }

    // request opens a stream, and sends a request's head (and body, if any).
    http2::stream_id
    request(std::string_view method, std::string_view path, std::string_view body = {}, bool end = true)
    {
        const auto id = s.open_stream().value();

        const std::vector<hpack::field> head{
            {   .name = ":method",      .value = method},
            {   .name = ":scheme",      .value = "http"},
            {.name = ":authority", .value = "localhost"},
            {     .name = ":path",        .value = path}
        };
        REQUIRE_FALSE(s.submit_headers(id, head, body.empty() && end));
        if (!body.empty()) send(id, body, end);
        return id;
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

    // pump writes what's to be written, and reads and receives, until done() says so (or the connection ends).
    template<typename Done>
    net::coro::task<bool> pump(Done done)
    {
        std::vector<std::byte> buffer(16'384);
        std::vector<std::byte> pending;
        std::vector<std::byte> output;

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
            if (res.err) closed = true;

            const auto used = s.receive(pending, *this, std::chrono::steady_clock::now());
            if (!used.has_value())
            {
                closed = true;
                co_return done();
            }
            pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(*used));
        }
    }

    net::coro::task<bool> wait_for(http2::stream_id id)
    {
        return pump([this, id] { return responses[id].done; });
    }

    std::map<http2::stream_id, std::string> pending_body;
    std::map<http2::stream_id, bool>        pending_end;
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
