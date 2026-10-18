// fuzz_http2 checks that an HTTP/2 session (as a server, and as a client) doesn't crash on any frames, and that it
// receives them the same whether they arrive at once or a byte at a time: the same events, the same output (frames to
// send), and the same outcome. The input follows a preface and empty SETTINGS, so that the fuzzer starts past them.
#include "http/http2.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <format>
#include <print>
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

constexpr auto now = std::chrono::steady_clock::time_point{std::chrono::hours{1}};

// recorder records a session's events as text.
struct recorder final : http2::session_events
{
    std::string log;

    void
    on_headers(http2::stream_id stream, http2::block_kind kind, std::span<const hpack::field> fields, bool end) override
    {
        log += std::format("headers {} {} {}:", stream, static_cast<int>(kind), end);
        for (const auto& f : fields) log += std::format(" {}={}", f.name, f.value);
        log += '\n';
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

// outcome is what a session made of some input.
struct outcome
{
    std::string            events;
    std::vector<std::byte> output;
    std::error_code        failure;
    std::size_t            unused = 0; // what was left, as not whole frames

    bool operator==(const outcome&) const = default;
};

outcome at_once(http2::role role, std::span<const std::byte> input)
{
    http2::session s{role};
    recorder       events;
    outcome        out;

    const auto used = s.receive(input, events, now);
    if (used.has_value()) out.unused = input.size() - *used;
    else out.failure = used.error();

    out.events = std::move(events.log);
    s.take_output(out.output);
    return out;
}

outcome byte_at_a_time(http2::role role, std::span<const std::byte> input)
{
    http2::session         s{role};
    recorder               events;
    outcome                out;
    std::vector<std::byte> pending;

    for (const auto b : input)
    {
        pending.push_back(b);
        const auto used = s.receive(pending, events, now);
        if (!used.has_value())
        {
            out.failure = used.error();
            pending.clear();
            break;
        }
        pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(*used));
    }

    out.unused = pending.size();
    out.events = std::move(events.log);
    s.take_output(out.output);
    return out;
}

void check(http2::role role, std::span<const std::byte> input)
{
    const auto whole    = at_once(role, input);
    const auto bytewise = byte_at_a_time(role, input);
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
    const std::span frames{reinterpret_cast<const std::byte*>(data), size};

    // a server, receiving a client's preface and SETTINGS, then the input.
    std::vector<std::byte> input;
    const auto             preface = std::as_bytes(std::span{http2::preface});
    input.assign(preface.begin(), preface.end());
    http2::encode_settings({}, {}, input);
    input.insert(input.end(), frames.begin(), frames.end());
    check(http2::role::server, input);

    // a client, receiving a server's SETTINGS, then the input.
    check(http2::role::client, std::span{input}.subspan(preface.size()));
    return 0;
}
