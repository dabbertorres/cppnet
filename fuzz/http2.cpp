// fuzz_http2 checks that an HTTP/2 session (as a server, and as a client) doesn't crash on any frames, nor on what its
// application does between them, and that it receives frames the same whether they arrive at once or a byte at a
// time: the same events, the same output (frames to send), and the same outcome.
//
// The input is a script, of records that each start with an op byte (its low 3 bits):
// - 0 and 7: receive the next n bytes (n is the next byte; 0, or more than there are: the rest), as frames.
// - 1: time passes: the next byte's tens of milliseconds (for the token buckets).
// - 2: headers (the next byte: bits 0-3 choose a stream, 4-5 what, 6 ends the stream): a server responds (a 200, one
//      with a Content-Length of 3, a 103, or trailers), a client opens a stream for a request (GET, POST with a
//      Content-Length of 3, or HEAD) or sends trailers.
// - 3: data (the next byte: bits 0-3 choose a stream, 7 ends the stream; the one after, its size in 64s).
// - 4: reset (the next byte: bits 0-3 choose a stream, 4-7 the code).
// - 5: consume (the next byte chooses a stream; the one after, how much, in 256s).
// - 6: PING, or GOAWAY (the next byte: bit 0 which, 1 whether the last stream is the peer's last or the most, 4-7 the
//      code).
// A stream is chosen from those the application knows (a server's requests, a client's own), in order.
//
// The server's script follows the client's preface and empty SETTINGS, and the client's the server's empty SETTINGS,
// so that the fuzzer starts past them. The client opens streams before it receives: a GET (1), a POST with a
// Content-Length of 3 and no body yet (3), and a HEAD (5).
#include "http/http2.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <format>
#include <print>
#include <ratio>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "http/hpack.hpp"
#include "http/http2_session.hpp"

namespace
{

namespace http2 = net::http::http2;
namespace hpack = net::http::hpack;

using time_point = std::chrono::steady_clock::time_point;

constexpr auto start = time_point{std::chrono::hours{1}};

// recorder records a session's events as text, and the streams the application knows of.
struct recorder final : http2::session_events
{
    std::string                   log;
    std::vector<http2::stream_id> known;

    void
    on_headers(http2::stream_id stream, http2::block_kind kind, std::span<const hpack::field> fields, bool end) override
    {
        log += std::format("headers {} {} {}:", stream, static_cast<int>(kind), end);
        for (const auto& f : fields) log += std::format(" {}={}", f.name, f.value);
        log += '\n';

        if (std::ranges::find(known, stream) == known.end()) known.push_back(stream);
    }

    void on_data(http2::stream_id stream, std::span<const std::byte> data, bool end) override
    {
        log += std::format("data {} {} {}\n", stream, data.size(), end);
    }

    void on_stream_reset(http2::stream_id stream, std::error_code err) override
    {
        log += std::format("reset {} {}\n", stream, err.value());
    }

    void on_window_update(http2::stream_id stream) override { log += std::format("window {}\n", stream); }
    void on_settings(const http2::settings& /*peer*/) override { log += "settings\n"; }

    void on_goaway(http2::stream_id last, std::error_code err, std::string_view debug) override
    {
        log += std::format("goaway {} {} {}\n", last, err.value(), debug.size());
    }

    void on_ping_ack(std::span<const std::byte, 8> /*opaque*/) override { log += "ping ack\n"; }
};

// outcome is what a session made of a script.
struct outcome
{
    std::string            events;
    std::vector<std::byte> output;
    std::error_code        failure;
    std::size_t            unused = 0; // what was left, as not whole frames

    bool operator==(const outcome&) const = default;
};

// script is the fuzzer's input, read a byte at a time.
struct script
{
    std::span<const std::byte> in;

    [[nodiscard]] bool done() const noexcept { return in.empty(); }

    std::uint8_t next() noexcept
    {
        if (in.empty()) return 0;
        const auto b = std::to_integer<std::uint8_t>(in.front());
        in           = in.subspan(1);
        return b;
    }

    std::span<const std::byte> take(std::size_t n) noexcept
    {
        if (n == 0 || n > in.size()) n = in.size();
        const auto out = in.first(n);
        in             = in.subspan(n);
        return out;
    }
};

// runner runs a script against a session, receiving frames at once or a byte at a time.
class runner
{
public:
    runner(http2::role role, bool bytewise)
        : role{role}
        , bytewise{bytewise}
        , s{role}
    {
        if (role == http2::role::client)
        {
            open(get, true);
            open(post, false);
            open(head, true);
        }
    }

    outcome run(std::span<const std::byte> prefix, std::span<const std::byte> input)
    {
        receive(prefix);

        script sc{input};
        while (!sc.done())
        {
            step(sc);
            collect();
        }

        out.unused = pending.size();
        out.events = std::move(events.log);
        collect();
        return std::move(out);
    }

private:
    static constexpr std::array get{
        hpack::field{   .name = ":method",         .value = "GET"},
        hpack::field{   .name = ":scheme",       .value = "https"},
        hpack::field{.name = ":authority", .value = "example.com"},
        hpack::field{     .name = ":path",           .value = "/"},
    };
    static constexpr std::array post{
        hpack::field{       .name = ":method",        .value = "POST"},
        hpack::field{       .name = ":scheme",       .value = "https"},
        hpack::field{    .name = ":authority", .value = "example.com"},
        hpack::field{         .name = ":path",           .value = "/"},
        hpack::field{.name = "content-length",           .value = "3"},
    };
    static constexpr std::array head{
        hpack::field{   .name = ":method",        .value = "HEAD"},
        hpack::field{   .name = ":scheme",       .value = "https"},
        hpack::field{.name = ":authority", .value = "example.com"},
        hpack::field{     .name = ":path",           .value = "/"},
    };
    static constexpr std::array ok{
        hpack::field{.name = ":status", .value = "200"},
    };
    static constexpr std::array ok_with_length{
        hpack::field{       .name = ":status", .value = "200"},
        hpack::field{.name = "content-length",   .value = "3"},
    };
    static constexpr std::array early_hints{
        hpack::field{.name = ":status", .value = "103"},
    };
    static constexpr std::array trailers{
        hpack::field{.name = "x", .value = "y"},
    };

    void open(std::span<const hpack::field> fields, bool end_stream)
    {
        const auto id = s.open_stream();
        if (!id.has_value())
        {
            events.log += std::format("open {}\n", id.error().value());
            return;
        }

        events.known.push_back(*id);
        const auto err = s.submit_headers(*id, fields, end_stream);
        events.log += std::format("open {} {}\n", *id, err.value());
    }

    // stream is the stream a selector chooses: one the application knows, or (if it knows none) a client's.
    [[nodiscard]] http2::stream_id stream(std::uint8_t selector) const noexcept
    {
        if (events.known.empty()) return (2U * selector) + 1U;
        return events.known[selector % events.known.size()];
    }

    void headers(std::uint8_t arg)
    {
        using fields = std::span<const hpack::field>;

        const auto what = (arg >> 4U) & 3U;
        const bool end  = (arg & 0x40U) != 0;

        if (role == http2::role::client && what != 3)
        {
            const std::array<fields, 3> requests{get, post, head};
            open(requests.at(what), end);
            return;
        }

        const std::array<fields, 4> blocks{ok, ok_with_length, early_hints, trailers};
        const auto                  id = stream(arg & 0xfU);
        events.log += std::format("submit headers {} {}\n", id, s.submit_headers(id, blocks.at(what), end).value());
    }

    void submit_data(std::uint8_t arg, std::uint8_t size)
    {
        const auto id = stream(arg & 0xfU);
        data.assign(std::size_t{64} * size, std::byte{'x'});

        const auto sent = s.submit_data(id, data, (arg & 0x80U) != 0);
        events.log += sent.has_value() ? std::format("submit data {} {}\n", id, *sent)
                                       : std::format("submit data {} error {}\n", id, sent.error().value());
    }

    void step(script& sc)
    {
        switch (sc.next() & 7U)
        {
        case 0:
        case 7: receive(sc.take(sc.next())); break;

        case 1: now += std::chrono::milliseconds{10} * sc.next(); break;

        case 2: headers(sc.next()); break;

        case 3:
        {
            const auto arg = sc.next();
            submit_data(arg, sc.next());
            break;
        }

        case 4:
        {
            const auto arg = sc.next();
            s.reset(stream(arg & 0xfU), static_cast<http2::error_code>(arg >> 4U));
            break;
        }

        case 5:
        {
            const auto id = stream(sc.next());
            s.consume(id, std::size_t{256} * sc.next());
            break;
        }

        case 6:
        {
            const auto arg = sc.next();
            if ((arg & 1U) == 0)
            {
                const std::array<std::byte, 8> opaque{};
                s.ping(opaque);
                break;
            }

            const auto last = (arg & 2U) != 0 ? s.last_peer_stream() : http2::max_stream_id;
            s.goaway(last, static_cast<http2::error_code>(arg >> 4U));
            break;
        }

        default: break;
        }
    }

    // receive passes what's arrived to the session: at once, or a byte at a time, each time with what's left of the
    // last (as the session takes only whole frames).
    void receive(std::span<const std::byte> in)
    {
        if (out.failure) return;

        if (!bytewise)
        {
            pending.insert(pending.end(), in.begin(), in.end());
            take_used();
            return;
        }

        for (const auto b : in)
        {
            pending.push_back(b);
            if (!take_used()) return;
        }
    }

    bool take_used()
    {
        const auto used = s.receive(pending, events, now);
        if (!used.has_value())
        {
            out.failure = used.error();
            pending.clear();
            return false;
        }
        pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(*used));
        return true;
    }

    // collect adds what the session has to send to the outcome (which, taken, the session no longer counts as waiting).
    void collect()
    {
        s.take_output(buffer);
        out.output.insert(out.output.end(), buffer.begin(), buffer.end());
    }

    http2::role            role;
    bool                   bytewise;
    http2::session         s;
    recorder               events;
    outcome                out;
    time_point             now = start;
    std::vector<std::byte> pending;
    std::vector<std::byte> buffer;
    std::vector<std::byte> data;
};

void check(http2::role role, std::span<const std::byte> prefix, std::span<const std::byte> input)
{
    const auto whole    = runner{role, false}.run(prefix, input);
    const auto bytewise = runner{role, true}.run(prefix, input);
    if (whole == bytewise) return;

    std::println(stderr,
                 "PROPERTY FAILED: received differently a byte at a time ({})\n  at once:\n{}  bytewise:\n{}",
                 role == http2::role::server ? "server" : "client",
                 whole.events,
                 bytewise.events);
    std::abort();
}

}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    const std::span input{reinterpret_cast<const std::byte*>(data), size};

    // a server, receiving a client's preface and SETTINGS first.
    std::vector<std::byte> prefix;
    const auto             preface = std::as_bytes(std::span{http2::preface});
    prefix.assign(preface.begin(), preface.end());
    http2::encode_settings({}, {}, prefix);
    check(http2::role::server, prefix, input);

    // a client, receiving a server's SETTINGS first.
    check(http2::role::client, std::span{prefix}.subspan(preface.size()), input);
    return 0;
}
