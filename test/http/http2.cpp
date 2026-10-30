#include "http/http2.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "encoding/hex.hpp"
#include "http/hpack.hpp"
#include "http/http2_config.hpp"
#include "http/http2_session.hpp"
#include "http/priority.hpp"

namespace http2 = net::http::http2;
namespace hpack = net::http::hpack;

using namespace std::string_view_literals;

namespace
{

constexpr auto now = std::chrono::steady_clock::time_point{std::chrono::hours{1}};

std::string to_hex(std::span<const std::byte> bytes)
{
    auto hex = net::encoding::hex::encode(bytes);
    for (auto& c : hex) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return hex;
}

std::span<const std::byte> bytes_of(std::string_view s) { return std::as_bytes(std::span{s}); }

std::string as_string(std::span<const std::byte> data)
{
    return {reinterpret_cast<const char*>(data.data()), data.size()};
}

// recorder records a session's events as lines, for tests to compare.
struct recorder final : http2::session_events
{
    std::vector<std::string> log;

    void
    on_headers(http2::stream_id stream, http2::block_kind kind, std::span<const hpack::field> fields, bool end) override
    {
        constexpr std::array kinds{"head"sv, "informational"sv, "trailers"sv};

        auto line = std::format("headers {} {}{}", stream, kinds.at(static_cast<std::size_t>(kind)), end ? " end" : "");
        for (const auto& f : fields) line += std::format(" {}={}", f.name, f.value);
        log.push_back(std::move(line));
    }

    void on_header_list_too_large(http2::stream_id stream, bool end) override
    {
        log.push_back(std::format("too large {}{}", stream, end ? " end" : ""));
    }

    void on_data(http2::stream_id stream, std::span<const std::byte> data, bool end) override
    {
        log.push_back(std::format("data {} [{}]{}", stream, as_string(data), end ? " end" : ""));
    }

    void on_stream_reset(http2::stream_id stream, std::error_code err) override
    {
        log.push_back(std::format("reset {} {}", stream, err.message()));
    }

    void on_window_update(http2::stream_id stream) override { log.push_back(std::format("window {}", stream)); }
    void on_settings(const http2::settings& /*peer*/) override { log.emplace_back("settings"); }

    void on_goaway(http2::stream_id last, std::error_code err, std::string_view debug) override
    {
        log.push_back(std::format("goaway {} {} [{}]", last, err.message(), debug));
    }

    void on_ping_ack(std::span<const std::byte, 8> opaque) override
    {
        log.push_back(std::format("ping ack {}", to_hex(opaque)));
    }

    void on_priority_update(http2::stream_id stream, net::http::priority p) override
    {
        log.push_back(std::format("priority {} u={}{}", stream, p.urgency, p.incremental ? " i" : ""));
    }

    void on_origin(std::span<const std::string_view> origins) override
    {
        std::string line = "origin";
        for (const auto o : origins) line += std::format(" [{}]", o);
        log.push_back(std::move(line));
    }

    // take returns the lines recorded since it was last called, without window updates (which are noise unless a
    // test is about them).
    std::vector<std::string> take(bool windows = false)
    {
        std::vector<std::string> out;
        for (auto& line : log)
        {
            if (windows || !line.starts_with("window")) out.push_back(std::move(line));
        }
        log.clear();
        return out;
    }
};

// endpoint is a session and what it records.
struct endpoint
{
    explicit endpoint(http2::role r, const http2::session_config& config = {})
        : s{r, config}
    {}

    http2::session  s;
    recorder        events;
    std::error_code failure;

    // deliver passes bytes to the session, as a connection would: all of them, as the session takes only whole frames.
    void deliver(std::span<const std::byte> in)
    {
        std::vector<std::byte> pending(in.begin(), in.end());
        auto                   used = s.receive(pending, events, now);
        if (!used.has_value())
        {
            failure = used.error();
            return;
        }
        REQUIRE(*used == pending.size());
    }
};

// pump passes each side's output to the other until neither has any.
void pump(endpoint& a, endpoint& b)
{
    std::vector<std::byte> buffer;
    for (int i = 0; i < 100 && (a.s.has_output() || b.s.has_output()); ++i)
    {
        a.s.take_output(buffer);
        if (!buffer.empty() && !b.failure) b.deliver(buffer);

        b.s.take_output(buffer);
        if (!buffer.empty() && !a.failure) a.deliver(buffer);
    }
}

// connected is a client and server that have exchanged their prefaces and settings.
struct connected
{
    explicit connected(const http2::session_config& server_config = {}, const http2::session_config& client_config = {})
        : client{http2::role::client, client_config}
        , server{http2::role::server, server_config}
    {
        pump(client, server);
        client.events.take();
        server.events.take();
    }

    endpoint client;
    endpoint server;
};

std::vector<hpack::field> request(std::string_view method = "GET", std::string_view path = "/")
{
    return {
        {   .name = ":method",        .value = method},
        {   .name = ":scheme",       .value = "https"},
        {.name = ":authority", .value = "example.com"},
        {     .name = ":path",          .value = path}
    };
}

std::vector<hpack::field> response(std::string_view status = "200")
{
    return {
        {.name = ":status", .value = status}
    };
}

// raw is frames written by hand, as a misbehaving peer would send them.
struct raw
{
    std::vector<std::byte> bytes;
    hpack::encoder         encoder;

    // client_start is a client's preface and empty SETTINGS.
    raw& client_start()
    {
        const auto p = bytes_of(http2::preface);
        bytes.insert(bytes.end(), p.begin(), p.end());
        http2::encode_settings({}, {}, bytes);
        return *this;
    }

    raw& frame(http2::frame_type type, std::uint8_t flags, http2::stream_id stream, std::span<const std::byte> payload)
    {
        http2::encode_frame_header({.length = static_cast<std::uint32_t>(payload.size()),
                                    .type   = static_cast<std::uint8_t>(type),
                                    .flags  = flags,
                                    .stream = stream},
                                   bytes);
        bytes.insert(bytes.end(), payload.begin(), payload.end());
        return *this;
    }

    raw& headers(http2::stream_id stream, std::span<const hpack::field> fields, std::uint8_t flags)
    {
        std::vector<std::byte> block;
        encoder.encode(fields, block);
        return frame(http2::frame_type::headers, flags, stream, block);
    }
};

std::vector<std::byte> u32(std::uint32_t value)
{
    return {static_cast<std::byte>(value >> 24U),
            static_cast<std::byte>(value >> 16U),
            static_cast<std::byte>(value >> 8U),
            static_cast<std::byte>(value)};
}

std::uint32_t read_u32(std::span<const std::byte> in)
{
    return (std::to_integer<std::uint32_t>(in[0]) << 24U) | (std::to_integer<std::uint32_t>(in[1]) << 16U)
         | (std::to_integer<std::uint32_t>(in[2]) << 8U) | std::to_integer<std::uint32_t>(in[3]);
}

// frames_of lists out's frames of a type, as "stream code" for RST_STREAM, and "last-stream code" for GOAWAY.
std::vector<std::string> frames_of(std::span<const std::byte> out, http2::frame_type type)
{
    // a server's output starts with frames; a client's with the preface.
    if (as_string(out).starts_with(http2::preface)) out = out.subspan(http2::preface.size());

    std::vector<std::string> found;
    while (out.size() >= 9)
    {
        const auto h       = http2::decode_frame_header(out.first<9>());
        const auto payload = out.subspan(9, h.length);
        if (h.type == static_cast<std::uint8_t>(type))
        {
            if (type == http2::frame_type::rst_stream)
                found.push_back(std::format("{} {}", h.stream, read_u32(payload)));
            else if (type == http2::frame_type::goaway)
                found.push_back(std::format("{} {}", read_u32(payload), read_u32(payload.subspan(4))));
        }
        out = out.subspan(9 + h.length);
    }
    return found;
}

// goaway_code is the error code of the GOAWAY frame in out, or nullopt if there's none.
std::optional<std::uint32_t> goaway_code(std::span<const std::byte> out)
{
    // a server's output starts with frames; a client's with the preface.
    if (as_string(out).starts_with(http2::preface)) out = out.subspan(http2::preface.size());

    while (out.size() >= 9)
    {
        const auto h       = http2::decode_frame_header(out.first<9>());
        const auto payload = out.subspan(9, h.length);
        if (h.type == static_cast<std::uint8_t>(http2::frame_type::goaway)) return read_u32(payload.subspan(4));
        out = out.subspan(9 + h.length);
    }
    return std::nullopt;
}

constexpr auto end_both = static_cast<std::uint8_t>(http2::flags::end_headers | http2::flags::end_stream);

}

TEST_CASE("frame headers are 24-bit lengths, a type, flags and a 31-bit stream id", "[http][http2][frames]")
{
    const http2::frame_header h{.length = 0x01'0203, .type = 0x04, .flags = 0x05, .stream = 0x0607'0809};

    std::vector<std::byte> out;
    http2::encode_frame_header(h, out);
    CHECK(to_hex(out) == "010203040506070809");
    CHECK(http2::decode_frame_header(std::span{out}.first<9>()) == h);

    // the reserved bit is dropped.
    out[5] |= std::byte{0x80};
    CHECK(http2::decode_frame_header(std::span{out}.first<9>()).stream == 0x0607'0809);
}

TEST_CASE("control frames are encoded as RFC 9113 lays them out", "[http][http2][frames]")
{
    std::vector<std::byte> out;

    SECTION("SETTINGS: only what differs from the defaults")
    {
        http2::encode_settings({.enable_push = false, .initial_window_size = 1'000}, {}, out);
        CHECK(to_hex(out)
              == "00000c040000000000"
                 "000200000000"
                 "0004000003e8");
    }

    SECTION("SETTINGS acknowledgement")
    {
        http2::encode_settings_ack(out);
        CHECK(to_hex(out) == "000000040100000000");
    }

    SECTION("PING")
    {
        const std::array<std::byte, 8> opaque{std::byte{1},
                                              std::byte{2},
                                              std::byte{3},
                                              std::byte{4},
                                              std::byte{5},
                                              std::byte{6},
                                              std::byte{7},
                                              std::byte{8}};
        http2::encode_ping(opaque, true, out);
        CHECK(to_hex(out)
              == "000008060100000000"
                 "0102030405060708");
    }

    SECTION("GOAWAY")
    {
        http2::encode_goaway(7, 0xb, "calm", out);
        CHECK(to_hex(out)
              == "00000c070000000000"
                 "00000007"
                 "0000000b"
                 "63616c6d");
    }

    SECTION("RST_STREAM, WINDOW_UPDATE and DATA")
    {
        http2::encode_rst_stream(3, 8, out);
        http2::encode_window_update(0, 65'535, out);
        http2::encode_data(5, bytes_of("hi"), true, out);
        CHECK(to_hex(out)
              == "000004030000000003"
                 "00000008"
                 "000004080000000000"
                 "0000ffff"
                 "000002000100000005"
                 "6869");
    }
}

TEST_CASE("a header block too large for a frame continues in CONTINUATIONs", "[http][http2][frames]")
{
    const std::vector<std::byte> block(40, std::byte{0xaa});

    std::vector<std::byte> out;
    http2::encode_headers(1, block, true, 16, out);

    // HEADERS (END_STREAM), CONTINUATION, CONTINUATION (END_HEADERS): 16, 16 and 8.
    REQUIRE(out.size() == (3 * 9) + 40);
    CHECK(http2::decode_frame_header(std::span{out}.first<9>())
          == http2::frame_header{.length = 16, .type = 1, .flags = http2::flags::end_stream, .stream = 1});
    CHECK(http2::decode_frame_header(std::span{out}.subspan(25).first<9>())
          == http2::frame_header{.length = 16, .type = 9, .flags = 0, .stream = 1});
    CHECK(http2::decode_frame_header(std::span{out}.subspan(50).first<9>())
          == http2::frame_header{.length = 8, .type = 9, .flags = http2::flags::end_headers, .stream = 1});
}

TEST_CASE("a client and server exchange prefaces and settings", "[http][http2][session]")
{
    http2::session_config server_config;
    server_config.local.max_concurrent_streams = 7;

    endpoint client{http2::role::client};
    endpoint server{http2::role::server, server_config};

    std::vector<std::byte> out;
    client.s.take_output(out);
    REQUIRE(as_string(out).starts_with(http2::preface));

    server.deliver(out);
    CHECK(server.events.take() == std::vector<std::string>{"settings"});

    pump(client, server);
    CHECK(client.events.take() == std::vector<std::string>{"settings"});
    CHECK(client.s.peer_settings().max_concurrent_streams == 7);
    CHECK_FALSE(client.failure);
    CHECK_FALSE(server.failure);
}

TEST_CASE("a client opens streams up to the server's MAX_CONCURRENT_STREAMS, counting those reserved",
          "[http][http2][session][limits]")
{
    http2::session_config server_config;
    server_config.local.max_concurrent_streams = 2;

    endpoint client{http2::role::client};
    endpoint server{http2::role::server, server_config};
    pump(client, server);
    REQUIRE(client.s.peer_settings().max_concurrent_streams == 2);

    CHECK(client.s.can_open_stream());
    CHECK(client.s.can_open_stream(1));
    CHECK_FALSE(client.s.can_open_stream(2));

    REQUIRE(client.s.open_stream().has_value());
    CHECK(client.s.can_open_stream());
    CHECK_FALSE(client.s.can_open_stream(1));

    REQUIRE(client.s.open_stream().has_value());
    CHECK_FALSE(client.s.can_open_stream());
    CHECK(client.s.open_stream().error() == http2::error_code::refused_stream);

    // a server never opens streams.
    CHECK_FALSE(server.s.can_open_stream());
}

TEST_CASE("a request and its response", "[http][http2][session]")
{
    connected c;

    const auto id = c.client.s.open_stream();
    REQUIRE(id.has_value());
    CHECK(*id == 1);

    REQUIRE_FALSE(c.client.s.submit_headers(*id, request(), true));
    pump(c.client, c.server);
    CHECK(c.server.events.take()
          == std::vector<std::string>{"headers 1 head end :method=GET :scheme=https :authority=example.com :path=/"});

    auto fields = response();
    fields.push_back({.name = "content-length", .value = "5"});
    REQUIRE_FALSE(c.server.s.submit_headers(1, fields, false));
    REQUIRE(c.server.s.submit_data(1, bytes_of("hello"), true) == 5U);
    pump(c.client, c.server);

    CHECK(c.client.events.take()
          == std::vector<std::string>{"headers 1 head :status=200 content-length=5", "data 1 [hello] end"});
    CHECK(c.client.s.open_streams() == 0);
    CHECK(c.server.s.open_streams() == 0);
}

TEST_CASE("a request with a body, and trailers", "[http][http2][session]")
{
    connected  c;
    const auto id = c.client.s.open_stream().value();

    REQUIRE_FALSE(c.client.s.submit_headers(id, request("POST"), false));
    REQUIRE(c.client.s.submit_data(id, bytes_of("body"), false) == 4U);
    const std::vector<hpack::field> trailers{
        {.name = "checksum", .value = "abc"}
    };
    REQUIRE_FALSE(c.client.s.submit_headers(id, trailers, true));
    pump(c.client, c.server);

    CHECK(c.server.events.take()
          == std::vector<std::string>{"headers 1 head :method=POST :scheme=https :authority=example.com :path=/",
                                      "data 1 [body]",
                                      "headers 1 trailers end checksum=abc"});
}

TEST_CASE("informational responses come before the final one", "[http][http2][session]")
{
    connected  c;
    const auto id = c.client.s.open_stream().value();
    REQUIRE_FALSE(c.client.s.submit_headers(id, request(), true));
    pump(c.client, c.server);

    REQUIRE_FALSE(c.server.s.submit_headers(id, response("103"), false));
    REQUIRE_FALSE(c.server.s.submit_headers(id, response("204"), true));
    pump(c.client, c.server);

    CHECK(c.client.events.take()
          == std::vector<std::string>{"headers 1 informational :status=103", "headers 1 head end :status=204"});
}

TEST_CASE("data is sent as far as the windows allow, and the rest when they grow", "[http][http2][session][flow]")
{
    connected  c;
    const auto id = c.client.s.open_stream().value();
    REQUIRE_FALSE(c.client.s.submit_headers(id, request(), true));
    pump(c.client, c.server);
    c.server.events.take();
    REQUIRE_FALSE(c.server.s.submit_headers(id, response(), false));

    // the client's stream window is its initial 65535 (its connection window is 1 MiB).
    const std::string body(100'000, 'x');
    const auto        sent = c.server.s.submit_data(id, bytes_of(body), true);
    REQUIRE(sent == 65'535U);
    CHECK(c.server.s.send_window(id) == 0);

    pump(c.client, c.server);
    c.client.events.take();

    // consuming what arrived gives the window back, which the server's told of.
    c.client.s.consume(id, 65'535);
    pump(c.client, c.server);
    CHECK(c.server.events.take(true) == std::vector<std::string>{"window 1"});
    CHECK(c.server.s.send_window(id) == 65'535);

    const auto rest = c.server.s.submit_data(id, bytes_of(body).subspan(65'535), true);
    CHECK(rest == 100'000U - 65'535U);
    pump(c.client, c.server);
    CHECK(c.client.events.take().back().ends_with(" end"));
}

TEST_CASE("a change of INITIAL_WINDOW_SIZE moves open streams' windows", "[http][http2][session][flow]")
{
    connected  c;
    const auto id = c.client.s.open_stream().value();
    REQUIRE_FALSE(c.client.s.submit_headers(id, request(), true));
    pump(c.client, c.server);
    REQUIRE(c.server.s.send_window(id) == 65'535);

    raw                    r;
    std::vector<std::byte> payload{std::byte{0}, std::byte{4}};
    const auto             value = u32(1'000);
    payload.insert(payload.end(), value.begin(), value.end());
    r.frame(http2::frame_type::settings, 0, 0, payload);

    c.server.deliver(r.bytes);
    CHECK(c.server.s.send_window(id) == 1'000);
}

TEST_CASE("the connection's window is given back when the peer has run out, though less than half was read",
          "[http][http2][session][flow]")
{
    // a connection window that holds 4 streams' windows.
    http2::session_config config;
    config.connection_window = 4 * 65'535;
    connected c{config};

    // 4 bodies fill the streams' windows, and so the connection's.
    const std::string             body(65'535, 'x');
    std::vector<http2::stream_id> ids;
    for (int i = 0; i < 4; ++i)
    {
        const auto id = c.client.s.open_stream().value();
        REQUIRE_FALSE(c.client.s.submit_headers(id, request("POST"), false));
        REQUIRE(c.client.s.submit_data(id, bytes_of(body), false) == body.size());
        ids.push_back(id);
    }
    pump(c.client, c.server);
    c.server.events.take();

    // reading one of them (a quarter of the connection's window) gives it back to the connection too, so the stream
    // can go on while the others wait to be read.
    c.server.s.consume(ids.back(), body.size());
    pump(c.client, c.server);
    CHECK(c.client.s.send_window(ids.back()) == 65'535);
    CHECK(c.client.s.send_window(ids.front()) == 0);
}

TEST_CASE("a stream reset for its DATA gives the connection's window back", "[http][http2][session][flow]")
{
    struct test_case
    {
        std::string_view name;
        bool             content_length = false; // too long for its Content-Length, or for the stream's window
    };

    const auto tc =
        GENERATE(test_case{.name = "the stream's window"}, test_case{.name = "Content-Length", .content_length = true});
    CAPTURE(tc.name);

    // frames as large as a stream's window (larger, for flow control), on 20 streams: more than the connection's 1 MiB
    // window, if what each took of it weren't given back.
    http2::session_config config;
    config.local.max_frame_size = std::uint32_t{1} << 17;
    endpoint server{http2::role::server, config};

    raw r;
    r.client_start();

    auto fields = request("POST");
    if (tc.content_length) fields.push_back({.name = "content-length", .value = "1"});
    const std::vector<std::byte> data(tc.content_length ? 65'535 : 65'536, std::byte{'x'});

    for (http2::stream_id id = 1; id < 40; id += 2)
    {
        r.headers(id, fields, http2::flags::end_headers);
        r.frame(http2::frame_type::data, 0, id, data);
    }
    server.deliver(r.bytes);

    CHECK_FALSE(server.failure);

    std::vector<std::byte> out;
    server.s.take_output(out);
    CHECK(goaway_code(out) == std::nullopt);
}

TEST_CASE("a malformed request is reset, without the application seeing it", "[http][http2][session][malformed]")
{
    struct test_case
    {
        std::string_view          what;
        std::vector<hpack::field> fields;
    };

    auto       fields = request();
    const auto tc     = GENERATE_REF(
        test_case{
            .what   = "no :path",
            .fields = {fields[0], fields[1], fields[2]}
    },
        test_case{.what   = "an uppercase name",
                  .fields = {fields[0], fields[1], fields[2], fields[3], {.name = "Accept", .value = "*/*"}}},
        test_case{.what   = "a connection-specific field",
                  .fields = {fields[0], fields[1], fields[2], fields[3], {.name = "connection", .value = "close"}}},
        test_case{.what   = "TE other than trailers",
                  .fields = {fields[0], fields[1], fields[2], fields[3], {.name = "te", .value = "gzip"}}},
        test_case{.what   = "a pseudo-header after a field",
                  .fields = {fields[0], fields[1], fields[2], {.name = "accept", .value = "*/*"}, fields[3]}},
        test_case{.what   = "a repeated pseudo-header",
                  .fields = {fields[0], fields[1], fields[2], fields[3], fields[3]}},
        test_case{.what   = "an unknown pseudo-header",
                  .fields = {fields[0], fields[1], fields[2], fields[3], {.name = ":unknown", .value = "x"}}},
        test_case{.what   = "a value with a newline",
                  .fields = {fields[0], fields[1], fields[2], fields[3], {.name = "x", .value = "a\nb"}}},
        test_case{.what   = "a value with leading space",
                  .fields = {fields[0], fields[1], fields[2], fields[3], {.name = "x", .value = " a"}}},
        test_case{.what   = "a value with trailing space",
                  .fields = {fields[0], fields[1], fields[2], fields[3], {.name = "x", .value = "a "}}},
        test_case{.what   = "a value with a leading tab",
                  .fields = {fields[0], fields[1], fields[2], fields[3], {.name = "x", .value = "\ta"}}},
        test_case{.what   = "a value with a trailing tab",
                  .fields = {fields[0], fields[1], fields[2], fields[3], {.name = "x", .value = "a\t"}}},
        test_case{.what   = "a value of only a space",
                  .fields = {fields[0], fields[1], fields[2], fields[3], {.name = "x", .value = " "}}},
        test_case{.what   = "a pseudo-header's value with trailing space",
                  .fields = {fields[0], fields[1], fields[2], {.name = ":path", .value = "/ "}}},
        test_case{.what   = "a value with a control character",
                  .fields = {fields[0], fields[1], fields[2], fields[3], {.name = "x", .value = "a\x01b"}}},
        test_case{.what   = "a value with DEL",
                  .fields = {fields[0], fields[1], fields[2], fields[3], {.name = "x", .value = "a\x7f"}}},
        test_case{.what   = "Host disagreeing with :authority",
                  .fields = {fields[0], fields[1], fields[2], fields[3], {.name = "host", .value = "other.com"}}},
        test_case{.what   = "two Hosts",
                  .fields = {fields[0],
                             fields[1],
                             fields[2],
                             fields[3],
                             {.name = "host", .value = "example.com"},
                             {.name = "host", .value = "example.com"}}},
        test_case{.what   = "a response's pseudo-header",
                  .fields = {fields[0], fields[1], fields[2], fields[3], {.name = ":status", .value = "200"}}});
    CAPTURE(tc.what);

    connected  c;
    const auto id = c.client.s.open_stream().value();
    REQUIRE_FALSE(c.client.s.submit_headers(id, tc.fields, true));
    pump(c.client, c.server);

    CHECK(c.server.events.take().empty());
    CHECK(c.client.events.take() == std::vector<std::string>{"reset 1 protocol error"});
    CHECK_FALSE(c.server.failure);
}

TEST_CASE("a body that disagrees with its Content-Length is reset", "[http][http2][session][malformed]")
{
    connected  c;
    const auto id = c.client.s.open_stream().value();

    auto fields = request("POST");
    fields.push_back({.name = "content-length", .value = "10"});
    REQUIRE_FALSE(c.client.s.submit_headers(id, fields, false));

    SECTION("shorter")
    {
        REQUIRE(c.client.s.submit_data(id, bytes_of("short"), true) == 5U);
    }

    SECTION("longer")
    {
        REQUIRE(c.client.s.submit_data(id, bytes_of("much longer than ten"), true) == 20U);
    }

    pump(c.client, c.server);
    const auto server_events = c.server.events.take();
    REQUIRE_FALSE(server_events.empty());
    CHECK(server_events.back() == "reset 1 protocol error");
    CHECK(c.client.events.take() == std::vector<std::string>{"reset 1 protocol error"});
}

TEST_CASE("a response to HEAD may promise a body it doesn't have", "[http][http2][session]")
{
    connected  c;
    const auto id = c.client.s.open_stream().value();
    REQUIRE_FALSE(c.client.s.submit_headers(id, request("HEAD"), true));
    pump(c.client, c.server);
    c.server.events.take();

    auto fields = response();
    fields.push_back({.name = "content-length", .value = "100"});
    REQUIRE_FALSE(c.server.s.submit_headers(id, fields, true));
    pump(c.client, c.server);

    CHECK(c.client.events.take() == std::vector<std::string>{"headers 1 head end :status=200 content-length=100"});
}

TEST_CASE("a response to HEAD may end after its head, short of its Content-Length", "[http][http2][session]")
{
    connected  c;
    const auto id = c.client.s.open_stream().value();
    REQUIRE_FALSE(c.client.s.submit_headers(id, request("HEAD"), true));
    pump(c.client, c.server);
    c.server.events.take();

    auto fields = response();
    fields.push_back({.name = "content-length", .value = "100"});
    REQUIRE_FALSE(c.server.s.submit_headers(id, fields, false));

    const std::vector<hpack::field> trailers{
        {.name = "x", .value = "y"}
    };
    const auto by_trailers = GENERATE(false, true);
    CAPTURE(by_trailers);
    if (by_trailers) REQUIRE_FALSE(c.server.s.submit_headers(id, trailers, true));
    else REQUIRE(c.server.s.submit_data(id, {}, true) == 0U);
    pump(c.client, c.server);

    CHECK(c.client.events.take()
          == std::vector<std::string>{"headers 1 head :status=200 content-length=100",
                                      by_trailers ? "headers 1 trailers end x=y" : "data 1 [] end"});
}

TEST_CASE("GOAWAY refuses the streams after its last, which may be retried", "[http][http2][session]")
{
    connected c;
    for (int i = 0; i < 3; ++i)
        REQUIRE_FALSE(c.client.s.submit_headers(c.client.s.open_stream().value(), request(), true));
    pump(c.client, c.server);
    c.server.events.take();

    c.server.s.goaway(1, http2::error_code::no_error, "bye");
    pump(c.client, c.server);

    CHECK(c.client.events.take()
          == std::vector<std::string>{"goaway 1 no error [bye]", "reset 3 refused stream", "reset 5 refused stream"});
    CHECK(c.client.s.open_stream().error() == http2::error_code::refused_stream);
}

TEST_CASE("the server refuses streams past its MAX_CONCURRENT_STREAMS", "[http][http2][session][limits]")
{
    http2::session_config config;
    config.local.max_concurrent_streams = 1;
    endpoint server{http2::role::server, config};

    raw r;
    r.client_start().headers(1, request(), http2::flags::end_headers).headers(3, request(), http2::flags::end_headers);
    server.deliver(r.bytes);

    const auto events = server.events.take();
    REQUIRE(events.size() == 2);
    CHECK(events[1].starts_with("headers 1 head"));

    // stream 3 was refused: RST_STREAM(REFUSED_STREAM) is in the output.
    std::vector<std::byte> out;
    server.s.take_output(out);
    CHECK(to_hex(out).contains("000004030000000003"
                               "00000007"));
}

TEST_CASE("the server's defenses", "[http][http2][session][limits]")
{
    http2::session_config config;
    config.peer_resets         = {.burst = 5, .rate = 1};
    config.max_pending_replies = 5;
    endpoint server{http2::role::server, config};

    raw r;
    r.client_start();

    SECTION("Rapid Reset: streams opened and reset, faster than allowed")
    {
        for (http2::stream_id id = 1; id < 20; id += 2)
        {
            r.headers(id, request(), http2::flags::end_headers);
            r.frame(http2::frame_type::rst_stream, 0, id, u32(8));
        }
    }

    SECTION("a PING flood, with the replies never taken")
    {
        const std::array<std::byte, 8> opaque{};
        for (int i = 0; i < 10; ++i) r.frame(http2::frame_type::ping, 0, 0, opaque);
    }

    SECTION("a CONTINUATION flood: more CONTINUATIONs than allowed")
    {
        r.headers(1, request(), 0);
        for (int i = 0; i < 9; ++i) r.frame(http2::frame_type::continuation, 0, 1, {});
    }

    server.deliver(r.bytes);
    CHECK(server.failure == http2::error_code::enhance_your_calm);
}

TEST_CASE("connection errors fail the session with GOAWAY", "[http][http2][session][malformed]")
{
    struct test_case
    {
        std::string_view  what;
        raw               frames;
        http2::error_code want;
    };

    const std::array<std::byte, 8> opaque{};

    raw no_preface;
    no_preface.bytes.assign(24, std::byte{'x'});

    raw ping_first;
    {
        const auto p = bytes_of(http2::preface);
        ping_first.bytes.assign(p.begin(), p.end());
        ping_first.frame(http2::frame_type::ping, 0, 0, opaque);
    }

    raw interleaved;
    interleaved.client_start().headers(1, request(), 0).frame(http2::frame_type::ping, 0, 0, opaque);

    raw big_frame;
    big_frame.client_start().frame(http2::frame_type::data, 0, 1, std::vector<std::byte>(16'385));

    raw idle_data;
    idle_data.client_start().frame(http2::frame_type::data, 0, 5, bytes_of("x"));

    raw zero_window;
    zero_window.client_start().frame(http2::frame_type::window_update, 0, 0, u32(0));

    raw bad_hpack;
    bad_hpack.client_start().frame(http2::frame_type::headers, end_both, 1, std::vector<std::byte>{std::byte{0x80}});

    raw push;
    push.client_start().frame(http2::frame_type::push_promise, http2::flags::end_headers, 1, u32(2));

    raw bad_window_setting;
    {
        std::vector<std::byte> payload{std::byte{0}, std::byte{4}};
        const auto             value = u32(0x8000'0000);
        payload.insert(payload.end(), value.begin(), value.end());
        bad_window_setting.client_start().frame(http2::frame_type::settings, 0, 0, payload);
    }

    raw bad_frame_size_setting;
    {
        std::vector<std::byte> payload{std::byte{0}, std::byte{5}};
        const auto             value = u32(100);
        payload.insert(payload.end(), value.begin(), value.end());
        bad_frame_size_setting.client_start().frame(http2::frame_type::settings, 0, 0, payload);
    }

    const auto tc = GENERATE_REF(
        test_case{.what = "not the preface", .frames = no_preface, .want = http2::error_code::protocol_error},
        test_case{.what   = "a first frame that isn't SETTINGS",
                  .frames = ping_first,
                  .want   = http2::error_code::protocol_error},
        test_case{.what   = "a frame inside a header block",
                  .frames = interleaved,
                  .want   = http2::error_code::protocol_error},
        test_case{.what   = "a frame larger than MAX_FRAME_SIZE",
                  .frames = big_frame,
                  .want   = http2::error_code::frame_size_error},
        test_case{.what = "DATA on an idle stream", .frames = idle_data, .want = http2::error_code::protocol_error},
        test_case{.what   = "a WINDOW_UPDATE of 0 for the connection",
                  .frames = zero_window,
                  .want   = http2::error_code::protocol_error},
        test_case{.what   = "a header block HPACK can't decode",
                  .frames = bad_hpack,
                  .want   = http2::error_code::compression_error},
        test_case{.what = "PUSH_PROMISE from a client", .frames = push, .want = http2::error_code::protocol_error},
        test_case{.what   = "INITIAL_WINDOW_SIZE past 2^31-1",
                  .frames = bad_window_setting,
                  .want   = http2::error_code::flow_control_error},
        test_case{.what   = "MAX_FRAME_SIZE below 16384",
                  .frames = bad_frame_size_setting,
                  .want   = http2::error_code::protocol_error});
    CAPTURE(tc.what);

    endpoint server{http2::role::server};
    server.deliver(tc.frames.bytes);
    CHECK(server.failure == tc.want);
    CHECK(server.s.failed());

    // GOAWAY, with the error, is in the output.
    std::vector<std::byte> out;
    server.s.take_output(out);
    CHECK(goaway_code(out) == static_cast<std::uint32_t>(tc.want));
}

TEST_CASE("PING is answered with its payload", "[http][http2][session]")
{
    connected c;

    const std::array<std::byte, 8> opaque{std::byte{9},
                                          std::byte{8},
                                          std::byte{7},
                                          std::byte{6},
                                          std::byte{5},
                                          std::byte{4},
                                          std::byte{3},
                                          std::byte{2}};
    c.client.s.ping(opaque);
    pump(c.client, c.server);
    CHECK(c.client.events.take() == std::vector<std::string>{"ping ack 0908070605040302"});
}

TEST_CASE("frames split anywhere are received the same", "[http][http2][session]")
{
    connected  c;
    const auto id = c.client.s.open_stream().value();
    REQUIRE_FALSE(c.client.s.submit_headers(id, request("POST"), false));
    REQUIRE(c.client.s.submit_data(id, bytes_of("some body"), true) == 9U);

    std::vector<std::byte> out;
    c.client.s.take_output(out);

    // fed a byte at a time: each receive() takes only whole frames, leaving the rest to be passed again.
    std::vector<std::byte> pending;
    for (const auto b : out)
    {
        pending.push_back(b);
        const auto used = c.server.s.receive(pending, c.server.events, now);
        REQUIRE(used.has_value());
        pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(*used));
    }

    CHECK(pending.empty());
    CHECK(c.server.events.take()
          == std::vector<std::string>{"headers 1 head :method=POST :scheme=https :authority=example.com :path=/",
                                      "data 1 [some body] end"});
}

TEST_CASE("a reset stream's late frames are ignored", "[http][http2][session]")
{
    connected  c;
    const auto id = c.client.s.open_stream().value();
    REQUIRE_FALSE(c.client.s.submit_headers(id, request("POST"), false));
    pump(c.client, c.server);
    c.server.events.take();

    // the server resets the stream, while the client is still sending its body.
    c.server.s.reset(id, http2::error_code::cancel);
    REQUIRE(c.client.s.submit_data(id, bytes_of("late"), true) == 4U);
    pump(c.client, c.server);

    CHECK(c.server.events.take().empty());
    CHECK_FALSE(c.server.failure);
    CHECK(c.client.events.take() == std::vector<std::string>{"reset 1 cancel"});
}

TEST_CASE("a malformed PRIORITY resets an open stream, and only counts against one that isn't",
          "[http][http2][session][malformed]")
{
    struct test_case
    {
        std::string_view       what;
        std::vector<std::byte> payload; // for stream 1 (other streams' have their ids instead)
        http2::error_code      want;
    };

    const auto tc = GENERATE(
        test_case{
            .what    = "depending on itself",
            .payload = {std::byte{0}, std::byte{0}, std::byte{0}, std::byte{1}, std::byte{16}},
            .want    = http2::error_code::protocol_error
    },
        test_case{.what    = "the wrong size",
                  .payload = {std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}},
                  .want    = http2::error_code::frame_size_error});
    CAPTURE(tc.what);

    const auto for_stream = [&](http2::stream_id id)
    {
        auto payload = tc.payload;
        if (payload.size() == 5) payload[3] = static_cast<std::byte>(id);
        return payload;
    };

    endpoint server{http2::role::server};
    raw      r;
    r.client_start();
    r.headers(1, request("POST"), http2::flags::end_headers);
    r.frame(http2::frame_type::priority, 0, 1, for_stream(1));
    r.frame(http2::frame_type::priority, 0, 3, for_stream(3)); // idle
    server.deliver(r.bytes);

    // the open stream is reset, and its reset reported.
    CHECK_FALSE(server.failure);
    CHECK(server.events.take()
          == std::vector<std::string>{"settings",
                                      "headers 1 head :method=POST :scheme=https :authority=example.com :path=/",
                                      std::format("reset 1 {}", make_error_code(tc.want).message())});

    // only it: RST_STREAM on an idle stream is a connection error for the client.
    std::vector<std::byte> out;
    server.s.take_output(out);

    std::vector<std::string> resets;
    for (std::span<const std::byte> rest = out; rest.size() >= 9;)
    {
        const auto h = http2::decode_frame_header(rest.first<9>());
        if (h.type == static_cast<std::uint8_t>(http2::frame_type::rst_stream))
            resets.push_back(std::format("{} {}", h.stream, std::to_integer<int>(rest[12])));
        rest = rest.subspan(9 + h.length);
    }
    CHECK(resets == std::vector<std::string>{std::format("1 {}", static_cast<int>(tc.want))});
}

TEST_CASE("a Host that differs from :authority only in case agrees with it", "[http][http2][session]")
{
    connected  c;
    const auto id     = c.client.s.open_stream().value();
    auto       fields = request();
    fields.push_back({.name = "host", .value = "Example.COM"});
    REQUIRE_FALSE(c.client.s.submit_headers(id, fields, true));
    pump(c.client, c.server);

    CHECK(c.server.events.take()
          == std::vector<std::string>{
              "headers 1 head end :method=GET :scheme=https :authority=example.com :path=/ host=Example.COM"});
    CHECK(c.client.events.take().empty());
}

TEST_CASE("a value may have spaces and tabs inside it", "[http][http2][session]")
{
    connected  c;
    const auto id     = c.client.s.open_stream().value();
    auto       fields = request();
    fields.push_back({.name = "x", .value = "a\tb"});
    fields.push_back({.name = "y", .value = "a b  c"});
    REQUIRE_FALSE(c.client.s.submit_headers(id, fields, true));
    pump(c.client, c.server);

    CHECK(c.server.events.take()
          == std::vector<std::string>{
              "headers 1 head end :method=GET :scheme=https :authority=example.com :path=/ x=a\tb y=a b  c"});
    CHECK(c.client.events.take().empty());
}

TEST_CASE("trailers that end a body short of its Content-Length reset the stream", "[http][http2][session][malformed]")
{
    connected  c;
    const auto id = c.client.s.open_stream().value();

    auto fields = request("POST");
    fields.push_back({.name = "content-length", .value = "10"});
    REQUIRE_FALSE(c.client.s.submit_headers(id, fields, false));
    REQUIRE(c.client.s.submit_data(id, bytes_of("short"), false) == 5U);
    const std::vector<hpack::field> trailers{
        {.name = "checksum", .value = "abc"}
    };
    REQUIRE_FALSE(c.client.s.submit_headers(id, trailers, true));
    pump(c.client, c.server);

    // the trailers aren't reported: the body ended short.
    CHECK(c.server.events.take()
          == std::vector<std::string>{
              "headers 1 head :method=POST :scheme=https :authority=example.com :path=/ content-length=10",
              "data 1 [short]",
              "reset 1 protocol error"});
    CHECK(c.client.events.take() == std::vector<std::string>{"reset 1 protocol error"});
    CHECK_FALSE(c.server.failure);
}

TEST_CASE("a connection error's GOAWAY doesn't raise the last stream of one sent before",
          "[http][http2][session][goaway]")
{
    endpoint server{http2::role::server};

    raw r;
    r.client_start().headers(1, request(), end_both);
    server.deliver(r.bytes);
    server.s.goaway(1, http2::error_code::no_error);

    // a stream after the GOAWAY's last (ignored), then a connection error.
    raw more;
    more.headers(3, request(), end_both).frame(http2::frame_type::window_update, 0, 0, u32(0));
    server.deliver(more.bytes);
    REQUIRE(server.failure == http2::error_code::protocol_error);

    std::vector<std::byte> out;
    server.s.take_output(out);
    CHECK(frames_of(out, http2::frame_type::goaway)
          == std::vector<std::string>{"1 0", std::format("1 {}", static_cast<int>(http2::error_code::protocol_error))});
}

TEST_CASE("the frames of streams after this side's GOAWAY are ignored, not counted as errors",
          "[http][http2][session][goaway]")
{
    // no glitches allowed, and a connection window that what's ignored would use up if it weren't given back.
    http2::session_config config;
    config.glitches          = {.burst = 0, .rate = 0};
    config.connection_window = 65'535;
    endpoint server{http2::role::server, config};

    raw r;
    r.client_start().headers(1, request("POST"), http2::flags::end_headers);
    server.deliver(r.bytes);
    server.s.goaway(1, http2::error_code::no_error);
    server.events.take();

    // stream 3's head adds a field to HPACK's dynamic table (a literal with incremental indexing: "x: y"), and its body
    // and trailers follow; then stream 1's trailers refer to the field, which they can only if stream 3's head was
    // decoded.
    raw more;
    more.frame(http2::frame_type::headers,
               http2::flags::end_headers,
               3,
               std::vector<std::byte>{std::byte{0x40}, std::byte{1}, std::byte{'x'}, std::byte{1}, std::byte{'y'}});
    const std::vector<std::byte> data(16'384, std::byte{'x'});
    for (int i = 0; i < 5; ++i) more.frame(http2::frame_type::data, 0, 3, data);
    more.headers(3,
                 std::vector<hpack::field>{
                     {.name = "checksum", .value = "abc"}
    },
                 end_both);
    more.frame(http2::frame_type::headers, end_both, 1, std::vector<std::byte>{std::byte{0xbe}});
    server.deliver(more.bytes);

    CHECK_FALSE(server.failure);
    CHECK(server.events.take() == std::vector<std::string>{"headers 1 trailers end x=y"});

    std::vector<std::byte> out;
    server.s.take_output(out);
    CHECK(frames_of(out, http2::frame_type::rst_stream).empty());
}

TEST_CASE("HEADERS on a stream whose reset is too old to remember is the stream's error, not the connection's",
          "[http][http2][session][malformed]")
{
    http2::session_config config;
    config.local.max_concurrent_streams = 200;
    endpoint server{http2::role::server, config};

    // 130 streams opened, and reset by the server: stream 1's reset is forgotten.
    raw r;
    r.client_start();
    for (http2::stream_id id = 1; id < 260; id += 2) r.headers(id, request("POST"), http2::flags::end_headers);
    server.deliver(r.bytes);
    for (http2::stream_id id = 1; id < 260; id += 2) server.s.reset(id, http2::error_code::cancel);

    std::vector<std::byte> out;
    server.s.take_output(out);
    server.events.take();

    raw more;
    more.headers(1,
                 std::vector<hpack::field>{
                     {.name = "checksum", .value = "abc"}
    },
                 end_both);
    server.deliver(more.bytes);

    CHECK_FALSE(server.failure);
    CHECK(server.events.take().empty());
    server.s.take_output(out);
    CHECK(frames_of(out, http2::frame_type::rst_stream)
          == std::vector<std::string>{std::format("1 {}", static_cast<int>(http2::error_code::stream_closed))});
}

TEST_CASE("HEADERS whose priority depends on its own stream resets it, keeping HPACK in step",
          "[http][http2][session][malformed]")
{
    endpoint server{http2::role::server};

    // stream 1's head, with a priority depending on stream 1, adds "x: y" to HPACK's dynamic table (a literal with
    // incremental indexing); stream 3's head refers to it.
    std::vector<std::byte> payload{std::byte{0}, std::byte{0}, std::byte{0}, std::byte{1}, std::byte{16}};
    hpack::encoder         encoder;
    encoder.encode(request(), payload);
    payload.insert(payload.end(), {std::byte{0x40}, std::byte{1}, std::byte{'x'}, std::byte{1}, std::byte{'y'}});

    std::vector<std::byte> block;
    encoder.encode(request(), block);
    block.push_back(std::byte{0xbe});

    raw r;
    r.client_start()
        .frame(http2::frame_type::headers, end_both | http2::flags::priority, 1, payload)
        .frame(http2::frame_type::headers, end_both, 3, block);
    server.deliver(r.bytes);

    CHECK_FALSE(server.failure);
    CHECK(
        server.events.take()
        == std::vector<std::string>{"settings",
                                    "headers 3 head end :method=GET :scheme=https :authority=example.com :path=/ x=y"});

    std::vector<std::byte> out;
    server.s.take_output(out);
    CHECK(frames_of(out, http2::frame_type::rst_stream)
          == std::vector<std::string>{std::format("1 {}", static_cast<int>(http2::error_code::protocol_error))});
}

TEST_CASE("a header block may have as many CONTINUATIONs as the largest one this side takes needs",
          "[http][http2][session][limits]")
{
    // a list of up to 1 MiB, in frames of 16 KiB (which the peer may send, however large this side's may be): 64.
    http2::session_config config;
    config.local.max_header_list_size = std::uint32_t{1} << 20;
    config.local.max_frame_size       = std::uint32_t{1} << 20;
    endpoint server{http2::role::server, config};

    const std::string long_value(std::size_t{40} * 1'024, 'a');
    auto              fields = request();
    fields.push_back({.name = "x", .value = long_value});
    std::vector<std::byte> block;
    hpack::encoder{}.encode(fields, block);

    // in 1 KiB frames: more than 8 CONTINUATIONs.
    raw r;
    r.client_start();
    constexpr std::size_t frame = 1'024;
    for (std::size_t at = 0; at < block.size(); at += frame)
    {
        const auto first = at == 0;
        const auto last  = at + frame >= block.size();
        const auto part  = std::span{block}.subspan(at, std::min(frame, block.size() - at));
        r.frame(
            first ? http2::frame_type::headers : http2::frame_type::continuation,
            static_cast<std::uint8_t>((first ? http2::flags::end_stream : 0) | (last ? http2::flags::end_headers : 0)),
            1,
            part);
    }
    server.deliver(r.bytes);

    CHECK_FALSE(server.failure);
    CHECK(server.events.take().back().starts_with("headers 1 head end"));
}

namespace
{

// small_list is a session's config whose header lists are limited to 200 bytes (HPACK's sizes: a field's name and
// value, and 32), and its glitches to burst (so the one after the burst fails the connection).
http2::session_config small_list(double burst = 10)
{
    http2::session_config config;
    config.local.max_header_list_size = 200;
    config.glitches                   = {.burst = burst, .rate = 0};
    return config;
}

// long_field is a field that takes any list over 200 bytes, though its block (Huffman coded) is under.
hpack::field long_field()
{
    static const std::string value(200, 'a');
    return {.name = "x-long", .value = value};
}

std::vector<hpack::field> long_request(std::string_view method = "GET")
{
    auto fields = request(method);
    fields.push_back(long_field());
    return fields;
}

}

TEST_CASE("a request's head over the header list limit is reported without its fields, for the server to answer",
          "[http][http2][session][limits]")
{
    endpoint server{http2::role::server, small_list()};

    raw r;
    r.client_start();

    SECTION("ending the stream: the answer ends it")
    {
        r.headers(1, long_request(), end_both);
        server.deliver(r.bytes);

        CHECK_FALSE(server.failure);
        CHECK(server.events.take() == std::vector<std::string>{"settings", "too large 1 end"});

        // its stream is open for the answer, which ends it.
        REQUIRE(server.s.open_streams() == 1);
        CHECK_FALSE(server.s.submit_headers(1, response("431"), true));
        CHECK(server.s.open_streams() == 0);
    }

    SECTION("with a body to follow")
    {
        r.headers(1, long_request("POST"), http2::flags::end_headers);
        server.deliver(r.bytes);

        CHECK_FALSE(server.failure);
        CHECK(server.events.take() == std::vector<std::string>{"settings", "too large 1"});

        raw body;
        body.frame(http2::frame_type::data, http2::flags::end_stream, 1, bytes_of("body"));

        SECTION("the body's reported, as the request's")
        {
            server.deliver(body.bytes);
            CHECK(server.events.take() == std::vector<std::string>{"data 1 [body] end"});
        }

        SECTION("once answered and reset, its DATA is ignored")
        {
            REQUIRE_FALSE(server.s.submit_headers(1, response("431"), true));
            server.s.reset(1, http2::error_code::no_error);
            server.deliver(body.bytes);

            CHECK_FALSE(server.failure);
            CHECK(server.events.take().empty());

            std::vector<std::byte> out;
            server.s.take_output(out);
            CHECK(frames_of(out, http2::frame_type::rst_stream)
                  == std::vector<std::string>{std::format("1 {}", static_cast<int>(http2::error_code::no_error))});
        }

        SECTION("what follows its head is trailers")
        {
            const std::vector<hpack::field> trailers{
                {.name = "checksum", .value = "abc"}
            };
            raw more;
            more.headers(1, trailers, end_both);
            server.deliver(more.bytes);

            CHECK_FALSE(server.failure);
            CHECK(server.events.take() == std::vector<std::string>{"headers 1 trailers end checksum=abc"});
        }
    }

    // the block was decoded all the same: HPACK's table is in step, and nothing was sent for it.
    std::vector<std::byte> out;
    server.s.take_output(out);
    CHECK(frames_of(out, http2::frame_type::goaway).empty());
}

TEST_CASE("trailers over the header list limit reset the stream, as before", "[http][http2][session][limits]")
{
    connected  c{small_list()};
    const auto id = c.client.s.open_stream().value();
    REQUIRE_FALSE(c.client.s.submit_headers(id, request("POST"), false));
    const std::vector<hpack::field> trailers{long_field()};
    REQUIRE_FALSE(c.client.s.submit_headers(id, trailers, true));
    pump(c.client, c.server);

    CHECK(c.server.events.take()
          == std::vector<std::string>{"headers 1 head :method=POST :scheme=https :authority=example.com :path=/",
                                      "reset 1 protocol error"});
    CHECK(c.client.events.take() == std::vector<std::string>{"reset 1 protocol error"});
    CHECK_FALSE(c.server.failure);
}

TEST_CASE("a client resets a response's head over its header list limit, as before", "[http][http2][session][limits]")
{
    connected  c{{}, small_list()};
    const auto id = c.client.s.open_stream().value();
    REQUIRE_FALSE(c.client.s.submit_headers(id, request(), true));
    pump(c.client, c.server);
    c.server.events.take();

    auto fields = response();
    fields.push_back(long_field());
    REQUIRE_FALSE(c.server.s.submit_headers(id, fields, true));
    pump(c.client, c.server);

    CHECK(c.client.events.take() == std::vector<std::string>{"reset 1 protocol error"});
    CHECK_FALSE(c.client.failure);

    // (the server's done with the stream, both sides having ended it, so the reset's nothing to it.)
    CHECK(c.server.events.take().empty());
}

TEST_CASE("a header list over the limit is a glitch, whatever its block", "[http][http2][session][limits]")
{
    // one glitch allowed: the second list over the limit fails the connection.
    SECTION("requests' heads")
    {
        endpoint server{http2::role::server, small_list(1)};

        raw r;
        r.client_start().headers(1, long_request(), end_both).headers(3, long_request(), end_both);
        server.deliver(r.bytes);

        CHECK(server.events.take() == std::vector<std::string>{"settings", "too large 1 end"});
        CHECK(server.failure == http2::error_code::enhance_your_calm);
    }

    SECTION("trailers")
    {
        endpoint server{http2::role::server, small_list(1)};

        const std::vector<hpack::field> trailers{long_field()};
        raw                             r;
        r.client_start()
            .headers(1, request("POST"), http2::flags::end_headers)
            .headers(1, trailers, end_both)
            .headers(3, request("POST"), http2::flags::end_headers)
            .headers(3, trailers, end_both);
        server.deliver(r.bytes);

        CHECK(server.failure == http2::error_code::enhance_your_calm);
    }

    SECTION("responses' heads")
    {
        connected c{{}, small_list(1)};
        for (int i = 0; i < 2; ++i)
            REQUIRE_FALSE(c.client.s.submit_headers(c.client.s.open_stream().value(), request(), true));
        pump(c.client, c.server);

        auto fields = response();
        fields.push_back(long_field());
        REQUIRE_FALSE(c.server.s.submit_headers(1, fields, true));
        REQUIRE_FALSE(c.server.s.submit_headers(3, fields, true));
        pump(c.client, c.server);

        CHECK(c.client.failure == http2::error_code::enhance_your_calm);
    }
}

TEST_CASE("the session's encoder indexes within the smaller of the peer's table size and its cap, signalling changes",
          "[http][http2][session][hpack]")
{
    struct test_case
    {
        std::string_view           what;
        std::uint32_t              cap;
        std::vector<std::uint32_t> sizes; // the client's SETTINGS_HEADER_TABLE_SIZE, a SETTINGS frame each
        std::string_view           updates;
        bool                       indexes;
    };

    // NOTE: 0x20 is a size update of 0; 0x3f, then an integer's continuation octets, one of 31 or more.
    const auto tc = GENERATE(
        test_case{
            .what    = "the peer's initial size",
            .cap     = 4'096,
            .sizes   = {},
            .updates = "",
            .indexes = true
    },
        test_case{.what    = "the peer's larger: still 4096",
                  .cap     = 4'096,
                  .sizes   = {65'536},
                  .updates = "",
                  .indexes = true},
        test_case{.what = "the peer's smaller", .cap = 4'096, .sizes = {2'048}, .updates = "3fe10f", .indexes = true},
        test_case{.what    = "the peer's lowered, then raised past the cap",
                  .cap     = 4'096,
                  .sizes   = {100, 8'192},
                  .updates = "3f453fe11f",
                  .indexes = true},
        test_case{.what    = "the peer's raised, then lowered back",
                  .cap     = 4'096,
                  .sizes   = {8'192, 4'096},
                  .updates = "",
                  .indexes = true},
        test_case{.what = "the peer's 0", .cap = 4'096, .sizes = {0}, .updates = "20", .indexes = false},
        test_case{.what = "a cap of 0", .cap = 0, .sizes = {}, .updates = "20", .indexes = false},
        test_case{.what    = "a cap above 4096, and the peer's larger",
                  .cap     = 8'192,
                  .sizes   = {65'536},
                  .updates = "3fe13f",
                  .indexes = true},
        test_case{.what    = "a cap above 4096, and the peer's initial size",
                  .cap     = 8'192,
                  .sizes   = {},
                  .updates = "",
                  .indexes = true});
    CAPTURE(tc.what);

    http2::session_config config;
    config.encoder_table_size = tc.cap;
    endpoint server{http2::role::server, config};

    raw r;
    r.client_start();
    for (const auto size : tc.sizes) http2::encode_settings({.header_table_size = size}, {}, r.bytes);
    r.headers(1, request(), end_both).headers(3, request(), end_both);
    server.deliver(r.bytes);
    REQUIRE_FALSE(server.failure);

    const std::vector<hpack::field> fields{
        {     .name = ":status",        .value = "200"},
        {.name = "content-type", .value = "text/plain"},
        {      .name = "server",     .value = "cppnet"}
    };
    std::vector<std::string> blocks;
    for (const http2::stream_id id : {1U, 3U})
    {
        REQUIRE_FALSE(server.s.submit_headers(id, fields, true));

        std::vector<std::byte> out;
        server.s.take_output(out);
        while (out.size() >= 9)
        {
            const auto h = http2::decode_frame_header(std::span{out}.first<9>());
            if (h.type == static_cast<std::uint8_t>(http2::frame_type::headers))
                blocks.push_back(to_hex(std::span{out}.subspan(9, h.length)));
            out.erase(out.begin(), out.begin() + 9 + h.length);
        }
    }
    REQUIRE(blocks.size() == 2);

    // the first block starts with the size updates; content-type and server are literals with incremental indexing
    // (5f and 76: their static names), unless the table's 0 (0f10 and 0f27: without indexing); the second has them as
    // dynamic indexes (63 and 62, the newest), or as the first had them, without the updates.
    CHECK(blocks[0].starts_with(tc.updates));
    const auto first = std::string_view{blocks[0]}.substr(tc.updates.size());
    CHECK(first.starts_with(tc.indexes ? "885f" : "880f10"));
    if (tc.indexes) CHECK(blocks[1] == "88bfbe");
    else CHECK(blocks[1] == first);
}

namespace
{

// settings_in is the first SETTINGS frame's parameters in out, as "id=value".
std::vector<std::string> settings_in(std::span<const std::byte> out)
{
    // a server's output starts with frames; a client's with the preface.
    if (as_string(out).starts_with(http2::preface)) out = out.subspan(http2::preface.size());

    while (out.size() >= 9)
    {
        const auto h       = http2::decode_frame_header(out.first<9>());
        const auto payload = out.subspan(9, h.length);
        out                = out.subspan(9 + h.length);
        if (h.type != static_cast<std::uint8_t>(http2::frame_type::settings)) continue;

        std::vector<std::string> found;
        for (auto rest = payload; rest.size() >= 6; rest = rest.subspan(6))
        {
            const auto id = (std::to_integer<unsigned>(rest[0]) << 8U) | std::to_integer<unsigned>(rest[1]);
            found.push_back(std::format("{}={}", id, read_u32(rest.subspan(2))));
        }
        return found;
    }
    return {};
}

// setting is a SETTINGS frame's payload with one parameter.
std::vector<std::byte> setting(http2::setting_id id, std::uint32_t value)
{
    std::vector<std::byte> payload{static_cast<std::byte>(static_cast<unsigned>(id) >> 8U), static_cast<std::byte>(id)};
    const auto             v = u32(value);
    payload.insert(payload.end(), v.begin(), v.end());
    return payload;
}

// priority_update is a PRIORITY_UPDATE frame's payload: the prioritized stream, and a Priority field's value.
std::vector<std::byte> priority_update(http2::stream_id stream, std::string_view value)
{
    auto       payload = u32(stream);
    const auto bytes   = bytes_of(value);
    payload.insert(payload.end(), bytes.begin(), bytes.end());
    return payload;
}

}

TEST_CASE("both sides send SETTINGS_NO_RFC7540_PRIORITIES as 1, in their first SETTINGS",
          "[http][http2][session][priority]")
{
    endpoint client{http2::role::client};
    endpoint server{http2::role::server};

    std::vector<std::byte> out;
    client.s.take_output(out);
    CHECK(std::ranges::contains(settings_in(out), "9=1"));
    server.deliver(out);

    server.s.take_output(out);
    CHECK(std::ranges::contains(settings_in(out), "9=1"));
    client.deliver(out);

    pump(client, server);
    CHECK(client.s.peer_settings().no_rfc7540_priorities);
    CHECK(server.s.peer_settings().no_rfc7540_priorities);
    CHECK_FALSE(client.failure);
    CHECK_FALSE(server.failure);
}

TEST_CASE("SETTINGS_NO_RFC7540_PRIORITIES is 0 or 1, and never changes after the first SETTINGS",
          "[http][http2][session][priority]")
{
    struct test_case
    {
        std::string_view                        what;
        std::vector<std::vector<std::uint32_t>> frames; // each SETTINGS' values of the setting, after the first
        std::optional<std::uint32_t>            first;  // the first SETTINGS' (none: it's left out)
        bool                                    fails;
        bool                                    value = false;
    };

    const auto tc = GENERATE(
        test_case{.what = "1", .frames = {}, .first = 1, .fails = false, .value = true},
        test_case{.what = "0", .frames = {}, .first = 0, .fails = false},
        test_case{.what = "2", .frames = {}, .first = 2, .fails = true},
        test_case{.what = "2^32-1", .frames = {}, .first = 0xffff'ffff, .fails = true},
        test_case{.what = "1, then 1 again", .frames = {{1}}, .first = 1, .fails = false, .value = true},
        test_case{.what = "1, then left out", .frames = {{}}, .first = 1, .fails = false, .value = true},
        test_case{.what = "1, then 0", .frames = {{0}}, .first = 1, .fails = true},
        test_case{.what = "left out, then 0 (its initial value)", .frames = {{0}}, .first = {}, .fails = false},
        test_case{.what = "left out, then 1", .frames = {{1}}, .first = {}, .fails = true},
        test_case{.what = "0 then 1 in the first SETTINGS: the last", .frames = {}, .first = {}, .fails = false},
        test_case{.what = "a later SETTINGS with 1 then 0, after 1", .frames = {{1, 0}}, .first = 1, .fails = true});
    CAPTURE(tc.what);

    const auto role = GENERATE(http2::role::server, http2::role::client);
    CAPTURE(role == http2::role::server ? "server" : "client");

    // the peer's frames: a client's preface first, for a server.
    raw r;
    if (role == http2::role::server)
    {
        const auto p = bytes_of(http2::preface);
        r.bytes.insert(r.bytes.end(), p.begin(), p.end());
    }

    std::vector<std::byte> first;
    if (tc.first.has_value()) first = setting(http2::setting_id::no_rfc7540_priorities, *tc.first);
    if (tc.what.starts_with("0 then 1"))
    {
        first          = setting(http2::setting_id::no_rfc7540_priorities, 0);
        const auto one = setting(http2::setting_id::no_rfc7540_priorities, 1);
        first.insert(first.end(), one.begin(), one.end());
    }
    r.frame(http2::frame_type::settings, 0, 0, first);

    for (const auto& values : tc.frames)
    {
        std::vector<std::byte> payload;
        for (const auto v : values)
        {
            const auto one = setting(http2::setting_id::no_rfc7540_priorities, v);
            payload.insert(payload.end(), one.begin(), one.end());
        }
        r.frame(http2::frame_type::settings, 0, 0, payload);
    }

    endpoint e{role};
    e.deliver(r.bytes);
    if (tc.fails)
    {
        CHECK(e.failure == http2::error_code::protocol_error);
        return;
    }

    REQUIRE_FALSE(e.failure);
    CHECK(e.s.peer_settings().no_rfc7540_priorities == (tc.value || tc.what.starts_with("0 then 1")));
}

TEST_CASE("PRIORITY_UPDATE's connection errors", "[http][http2][session][priority][malformed]")
{
    struct test_case
    {
        std::string_view  what;
        raw               frames;
        http2::error_code want;
    };

    raw on_stream;
    on_stream.client_start()
        .headers(1, request(), http2::flags::end_headers)
        .frame(http2::frame_type::priority_update, 0, 1, priority_update(1, "u=1"));

    raw short_payload;
    short_payload.client_start().frame(http2::frame_type::priority_update, 0, 0, std::vector<std::byte>(3));

    raw stream_zero;
    stream_zero.client_start().frame(http2::frame_type::priority_update, 0, 0, priority_update(0, "u=1"));

    raw push_stream;
    push_stream.client_start().frame(http2::frame_type::priority_update, 0, 0, priority_update(2, "u=1"));

    raw in_block;
    in_block.client_start()
        .headers(1, request(), 0)
        .frame(http2::frame_type::priority_update, 0, 0, priority_update(1, "u=1"));

    const auto tc = GENERATE_REF(
        test_case{.what = "on a stream, not 0", .frames = on_stream, .want = http2::error_code::protocol_error},
        test_case{.what   = "shorter than a stream id",
                  .frames = short_payload,
                  .want   = http2::error_code::frame_size_error},
        test_case{.what = "for stream 0", .frames = stream_zero, .want = http2::error_code::protocol_error},
        test_case{.what   = "for an even (push) stream",
                  .frames = push_stream,
                  .want   = http2::error_code::protocol_error},
        test_case{.what = "inside a header block", .frames = in_block, .want = http2::error_code::protocol_error});
    CAPTURE(tc.what);

    endpoint server{http2::role::server};
    server.deliver(tc.frames.bytes);
    CHECK(server.failure == tc.want);

    std::vector<std::byte> out;
    server.s.take_output(out);
    CHECK(goaway_code(out) == static_cast<std::uint32_t>(tc.want));
}

TEST_CASE("a client takes PRIORITY_UPDATE as a connection error", "[http][http2][session][priority][malformed]")
{
    endpoint client{http2::role::client};
    REQUIRE(client.s.open_stream() == 1U);

    raw r;
    http2::encode_settings({}, {}, r.bytes);
    r.frame(http2::frame_type::priority_update, 0, 0, priority_update(1, "u=1"));
    client.deliver(r.bytes);
    CHECK(client.failure == http2::error_code::protocol_error);
}

TEST_CASE("PRIORITY_UPDATE for an open stream is reported, as a complete set", "[http][http2][session][priority]")
{
    endpoint server{http2::role::server};

    raw r;
    r.client_start()
        .headers(1, request("POST"), http2::flags::end_headers)
        .headers(3, request(), end_both)
        .frame(http2::frame_type::priority_update, 0, 0, priority_update(1, "u=5, i"))
        .frame(http2::frame_type::priority_update, 0, 0, priority_update(1, "i"))
        .frame(http2::frame_type::priority_update, 0, 0, priority_update(1, "u=0, x=?1;y, i=?0"))
        .frame(http2::frame_type::priority_update, 0, 0, priority_update(1, ""))
        .frame(http2::frame_type::priority_update, 0, 0, priority_update(3, "u=9, i=1, u=6"));
    server.deliver(r.bytes);
    REQUIRE_FALSE(server.failure);

    // what each leaves out is the default (u=3, not incremental), not what the stream had; stream 3 has ended its
    // request, but not been answered, so it's open.
    CHECK(server.events.take()
          == std::vector<std::string>{
              "settings",
              "headers 1 head :method=POST :scheme=https :authority=example.com :path=/",
              "headers 3 head end :method=GET :scheme=https :authority=example.com :path=/",
              "priority 1 u=5 i",
              "priority 1 u=3 i",
              "priority 1 u=0",
              "priority 1 u=3",
              "priority 3 u=6",
          });
}

TEST_CASE("PRIORITY_UPDATE for a stream not yet open is kept, and reported just after its head",
          "[http][http2][session][priority]")
{
    endpoint server{http2::role::server};

    auto with_priority = request();
    with_priority.push_back({.name = "priority", .value = "u=2"});

    raw r;
    r.client_start()
        .frame(http2::frame_type::priority_update, 0, 0, priority_update(1, "u=1"))
        .frame(http2::frame_type::priority_update, 0, 0, priority_update(1, "u=6, i"))
        .frame(http2::frame_type::priority_update, 0, 0, priority_update(5, "u=4"))
        .headers(1, with_priority, end_both)
        .headers(3, request(), end_both)
        .headers(5, request(), end_both);
    server.deliver(r.bytes);
    REQUIRE_FALSE(server.failure);

    // the latest of stream 1's, after its head (whose field it overrides, being the most recent signal); none for 3.
    CHECK(server.events.take()
          == std::vector<std::string>{
              "settings",
              "headers 1 head end :method=GET :scheme=https :authority=example.com :path=/ priority=u=2",
              "priority 1 u=6 i",
              "headers 3 head end :method=GET :scheme=https :authority=example.com :path=/",
              "headers 5 head end :method=GET :scheme=https :authority=example.com :path=/",
              "priority 5 u=4",
          });
}

TEST_CASE("a kept PRIORITY_UPDATE goes with its stream's head, if that's malformed or refused",
          "[http][http2][session][priority]")
{
    http2::session_config config;
    config.local.max_concurrent_streams = 2;
    endpoint server{http2::role::server, config};

    auto malformed = request();
    malformed.push_back({.name = "connection", .value = "close"});

    // stream 1's head is malformed (reset); stream 7's is refused, as 3 and 5 are open by then.
    raw r;
    r.client_start()
        .frame(http2::frame_type::priority_update, 0, 0, priority_update(1, "u=1"))
        .headers(1, malformed, end_both)
        .headers(3, request(), http2::flags::end_headers)
        .frame(http2::frame_type::priority_update, 0, 0, priority_update(7, "u=2"))
        .headers(5, request(), http2::flags::end_headers)
        .headers(7, request(), end_both);
    server.deliver(r.bytes);
    REQUIRE_FALSE(server.failure);

    CHECK(server.events.take()
          == std::vector<std::string>{
              "settings",
              "headers 3 head :method=GET :scheme=https :authority=example.com :path=/",
              "headers 5 head :method=GET :scheme=https :authority=example.com :path=/",
          });

    std::vector<std::byte> out;
    server.s.take_output(out);
    CHECK(frames_of(out, http2::frame_type::rst_stream) == std::vector<std::string>{"1 1", "7 7"});
}

TEST_CASE("PRIORITY_UPDATEs kept for streams not yet open are bounded, with the open ones, by MAX_CONCURRENT_STREAMS",
          "[http][http2][session][priority][limits]")
{
    http2::session_config config;
    config.local.max_concurrent_streams = 2;
    endpoint server{http2::role::server, config};

    raw r;
    r.client_start()
        .frame(http2::frame_type::priority_update, 0, 0, priority_update(3, "u=1"))
        .frame(http2::frame_type::priority_update, 0, 0, priority_update(5, "u=1"))
        .frame(http2::frame_type::priority_update, 0, 0, priority_update(5, "u=2")); // replaces, so doesn't count
    server.deliver(r.bytes);
    REQUIRE_FALSE(server.failure);
    server.events.take();

    SECTION("a third stream's is past the bound")
    {
        raw more;
        more.frame(http2::frame_type::priority_update, 0, 0, priority_update(7, "u=1"));
        server.deliver(more.bytes);
        CHECK(server.failure == http2::error_code::protocol_error);
    }

    SECTION("a stream opened past them closes theirs, so they're dropped; then it counts, with one more")
    {
        raw more;
        more.headers(7, request(), http2::flags::end_headers)
            .frame(http2::frame_type::priority_update, 0, 0, priority_update(9, "u=1"));
        server.deliver(more.bytes);
        REQUIRE_FALSE(server.failure);
        CHECK(server.events.take()
              == std::vector<std::string>{"headers 7 head :method=GET :scheme=https :authority=example.com :path=/"});

        raw past;
        past.frame(http2::frame_type::priority_update, 0, 0, priority_update(11, "u=1"));
        server.deliver(past.bytes);
        CHECK(server.failure == http2::error_code::protocol_error);
    }
}

TEST_CASE("PRIORITY_UPDATEs kept for streams not yet open are at most 128, whatever MAX_CONCURRENT_STREAMS",
          "[http][http2][session][priority][limits]")
{
    // past 128, they're ignored as glitches, of which one is allowed.
    http2::session_config config;
    config.local.max_concurrent_streams = http2::unlimited;
    config.glitches                     = {.burst = 1, .rate = 0};
    endpoint server{http2::role::server, config};

    raw r;
    r.client_start();
    for (http2::stream_id id = 1; id <= 2 * 129; id += 2)
        r.frame(http2::frame_type::priority_update, 0, 0, priority_update(id, "u=1"));
    server.deliver(r.bytes);
    REQUIRE_FALSE(server.failure);

    // the first 128 are kept (stream 1's among them), the 129th ignored.
    raw more;
    more.headers(1, request(), end_both).headers((2 * 129) - 1, request(), end_both);
    server.deliver(more.bytes);
    REQUIRE_FALSE(server.failure);
    CHECK(server.events.take()
          == std::vector<std::string>{
              "settings",
              "headers 1 head end :method=GET :scheme=https :authority=example.com :path=/",
              "priority 1 u=1",
              "headers 257 head end :method=GET :scheme=https :authority=example.com :path=/",
          });

    // the next ignored one is one glitch too many.
    raw past;
    past.frame(http2::frame_type::priority_update, 0, 0, priority_update(3, "u=1"));
    server.deliver(past.bytes);
    CHECK(server.failure == http2::error_code::enhance_your_calm);
}

TEST_CASE("PRIORITY_UPDATE for a closed stream, or whose value doesn't parse, is ignored as a glitch",
          "[http][http2][session][priority][limits]")
{
    // one glitch allowed.
    http2::session_config config;
    config.glitches = {.burst = 1, .rate = 0};
    endpoint server{http2::role::server, config};

    raw r;
    r.client_start().headers(1, request(), end_both).headers(3, request(), http2::flags::end_headers);
    server.deliver(r.bytes);
    REQUIRE_FALSE(server.s.submit_headers(1, response(), true)); // stream 1 is closed
    server.events.take();

    const auto first  = GENERATE(as<std::string_view>{}, "closed", "unparsable", "unparsable, idle");
    const auto second = GENERATE(as<std::string_view>{}, "closed", "unparsable", "unparsable, idle");
    CAPTURE(first, second);

    const auto frame = [](std::string_view what)
    {
        if (what == "closed") return priority_update(1, "u=1");
        if (what == "unparsable") return priority_update(3, "u=1, U=2");
        return priority_update(5, "u=");
    };

    raw once;
    once.frame(http2::frame_type::priority_update, 0, 0, frame(first));
    server.deliver(once.bytes);
    REQUIRE_FALSE(server.failure);

    raw twice;
    twice.frame(http2::frame_type::priority_update, 0, 0, frame(second));
    server.deliver(twice.bytes);
    CHECK(server.failure == http2::error_code::enhance_your_calm);

    // none reported, nor kept: stream 5's head has none after it.
    CHECK(server.events.take().empty());
}

TEST_CASE("PRIORITY_UPDATE for a stream after this side's GOAWAY is ignored, not counted",
          "[http][http2][session][priority][goaway]")
{
    http2::session_config config;
    config.glitches = {.burst = 0, .rate = 0};
    endpoint server{http2::role::server, config};

    raw r;
    r.client_start().headers(1, request(), http2::flags::end_headers);
    server.deliver(r.bytes);
    server.s.goaway(1, http2::error_code::no_error);
    server.events.take();

    raw more;
    more.frame(http2::frame_type::priority_update, 0, 0, priority_update(3, "u=1"))
        .headers(3, request(), end_both)
        .frame(http2::frame_type::priority_update, 0, 0, priority_update(3, "u=2"))
        .frame(http2::frame_type::priority_update, 0, 0, priority_update(1, "u=5"));
    server.deliver(more.bytes);
    REQUIRE_FALSE(server.failure);
    CHECK(server.events.take() == std::vector<std::string>{"priority 1 u=5"});
}

namespace
{

// frames_with_updates is out's PRIORITY_UPDATE frames, as "stream value" (a frame on a stream other than 0 as
// "on stream n"), and its other frames' types, as "type n": so where an update is among the frames is seen too.
std::vector<std::string> frames_with_updates(std::span<const std::byte> out)
{
    if (as_string(out).starts_with(http2::preface)) out = out.subspan(http2::preface.size());

    std::vector<std::string> found;
    while (out.size() >= 9)
    {
        const auto h       = http2::decode_frame_header(out.first<9>());
        const auto payload = out.subspan(9, h.length);
        out                = out.subspan(9 + h.length);

        if (h.type != static_cast<std::uint8_t>(http2::frame_type::priority_update))
        {
            found.push_back(std::format("type {}", h.type));
        }
        else if (h.stream != 0)
        {
            found.push_back(std::format("on stream {}", h.stream));
        }
        else
        {
            found.push_back(std::format("update {} [{}]", read_u32(payload), as_string(payload.subspan(4))));
        }
    }
    REQUIRE(out.empty());
    return found;
}

// updates_in is out's PRIORITY_UPDATE frames alone (see frames_with_updates()).
std::vector<std::string> updates_in(std::span<const std::byte> out)
{
    auto found = frames_with_updates(out);
    std::erase_if(found, [](const std::string& f) { return !f.starts_with("update"); });
    return found;
}

// with_priority is a request with a Priority field of these lines.
std::vector<hpack::field> with_priority(std::initializer_list<std::string_view> lines, std::string_view method = "GET")
{
    auto fields = request(method);
    for (const auto line : lines) fields.push_back({.name = "priority", .value = line});
    return fields;
}

// open_request opens a client's stream for a request with fields (ending it, unless end is false).
http2::stream_id open_request(endpoint& client, std::span<const hpack::field> fields, bool end = true)
{
    const auto id = client.s.open_stream();
    REQUIRE(id.has_value());
    REQUIRE_FALSE(client.s.submit_headers(*id, fields, end));
    return *id;
}

}

TEST_CASE("a client's PRIORITY_UPDATE is on stream 0, for its stream, with the priority's value",
          "[http][http2][session][priority][client]")
{
    connected  c;
    const auto id = open_request(c.client, request());

    CHECK(c.client.s.submit_priority_update(id, {.urgency = 5, .incremental = true}) == true);

    std::vector<std::byte> out;
    c.client.s.take_output(out);
    CHECK(frames_with_updates(out) == std::vector<std::string>{"type 1", "update 1 [u=5, i]"});

    // the server takes it as the stream's priority.
    c.server.deliver(out);
    REQUIRE_FALSE(c.server.failure);
    CHECK(c.server.events.take()
          == std::vector<std::string>{
              "headers 1 head end :method=GET :scheme=https :authority=example.com :path=/",
              "priority 1 u=5 i",
          });

    // a complete set: the default is "", and an urgency past 7 is 7.
    CHECK(c.client.s.submit_priority_update(id, {}) == true);
    c.client.s.take_output(out);
    CHECK(updates_in(out) == std::vector<std::string>{"update 1 []"});
    CHECK(c.client.s.submit_priority_update(id, {.urgency = 9}) == true);
    c.client.s.take_output(out);
    CHECK(updates_in(out) == std::vector<std::string>{"update 1 [u=7]"});
    c.server.deliver(out);
    CHECK(c.server.events.take() == std::vector<std::string>{"priority 1 u=7"});
}

TEST_CASE("a client's PRIORITY_UPDATE is sent only when it changes what the stream last said",
          "[http][http2][session][priority][client]")
{
    connected              c;
    std::vector<std::byte> out;

    SECTION("the request's field is what it said first")
    {
        const auto id = open_request(c.client, with_priority({"u=1"}));
        CHECK(c.client.s.submit_priority_update(id, {.urgency = 1}) == true);
        c.client.s.take_output(out);
        CHECK(updates_in(out).empty());

        CHECK(c.client.s.submit_priority_update(id, {.urgency = 1, .incremental = true}) == true);
        c.client.s.take_output(out);
        CHECK(updates_in(out) == std::vector<std::string>{"update 1 [u=1, i]"});

        // then that's what it said.
        CHECK(c.client.s.submit_priority_update(id, {.urgency = 1, .incremental = true}) == true);
        c.client.s.take_output(out);
        CHECK(updates_in(out).empty());
    }

    SECTION("a field of several lines is them joined")
    {
        const auto id = open_request(c.client, with_priority({"u=6", "i"}));
        CHECK(c.client.s.submit_priority_update(id, {.urgency = 6, .incremental = true}) == true);
        c.client.s.take_output(out);
        CHECK(updates_in(out).empty());
    }

    SECTION("what a field leaves out, or all of one that doesn't parse, is the default")
    {
        const auto partial   = open_request(c.client, with_priority({"i"}));
        const auto malformed = open_request(c.client, with_priority({"u=1, @"}));
        CHECK(c.client.s.submit_priority_update(partial, {.urgency = 3, .incremental = true}) == true);
        CHECK(c.client.s.submit_priority_update(malformed, {}) == true);
        c.client.s.take_output(out);
        CHECK(updates_in(out).empty());
    }

    SECTION("without a field, it's said nothing: the first update is sent, even the default")
    {
        const auto id = open_request(c.client, request());
        CHECK(c.client.s.submit_priority_update(id, {}) == true);
        c.client.s.take_output(out);
        CHECK(updates_in(out) == std::vector<std::string>{"update 1 []"});

        CHECK(c.client.s.submit_priority_update(id, {}) == true);
        c.client.s.take_output(out);
        CHECK(updates_in(out).empty());
    }
}

TEST_CASE("a client's PRIORITY_UPDATE not yet taken is replaced in place, or taken back",
          "[http][http2][session][priority][client]")
{
    connected              c;
    std::vector<std::byte> out;
    const auto             first  = open_request(c.client, with_priority({"u=1"}, "POST"), false);
    const auto             second = open_request(c.client, request());
    c.client.s.take_output(out);
    c.server.deliver(out);
    c.server.events.take();

    SECTION("by a later one, longer or shorter, where it was")
    {
        REQUIRE(c.client.s.submit_priority_update(first, {.urgency = 5}) == true);
        REQUIRE(c.client.s.submit_data(first, bytes_of("abc"), false) == 3U);
        REQUIRE(c.client.s.submit_priority_update(second, {.urgency = 0}) == true);
        REQUIRE(c.client.s.submit_priority_update(first, {.urgency = 6, .incremental = true}) == true);
        REQUIRE(c.client.s.submit_priority_update(second, {.urgency = 2}) == true);
        REQUIRE(c.client.s.submit_priority_update(first, {.urgency = 4}) == true);

        c.client.s.take_output(out);
        CHECK(frames_with_updates(out) == std::vector<std::string>{"update 1 [u=4]", "type 0", "update 3 [u=2]"});

        // what it says, the server takes.
        c.server.deliver(out);
        REQUIRE_FALSE(c.server.failure);
        CHECK(c.server.events.take() == std::vector<std::string>{"priority 1 u=4", "data 1 [abc]", "priority 3 u=2"});
    }

    SECTION("by one saying what was said before it")
    {
        REQUIRE(c.client.s.submit_priority_update(second, {.urgency = 0}) == true);
        REQUIRE(c.client.s.submit_priority_update(first, {.urgency = 5}) == true);
        REQUIRE(c.client.s.submit_priority_update(first, {.urgency = 1}) == true);

        c.client.s.take_output(out);
        CHECK(frames_with_updates(out) == std::vector<std::string>{"update 3 [u=0]"});

        // and it's what's said: the same again sends nothing, another does.
        REQUIRE(c.client.s.submit_priority_update(first, {.urgency = 1}) == true);
        c.client.s.take_output(out);
        CHECK(updates_in(out).empty());
        REQUIRE(c.client.s.submit_priority_update(first, {.urgency = 5}) == true);
        c.client.s.take_output(out);
        CHECK(updates_in(out) == std::vector<std::string>{"update 1 [u=5]"});
    }

    SECTION("once taken, the next is a frame of its own")
    {
        REQUIRE(c.client.s.submit_priority_update(first, {.urgency = 5}) == true);
        c.client.s.take_output(out);
        REQUIRE(c.client.s.submit_priority_update(first, {.urgency = 1}) == true);
        c.client.s.take_output(out);
        CHECK(updates_in(out) == std::vector<std::string>{"update 1 [u=1]"});
    }

    SECTION("when its stream ends: the server ends its response (the request's still being sent, or not), or it's "
            "reset")
    {
        const auto third = open_request(c.client, request());
        REQUIRE(c.client.s.submit_priority_update(second, {.urgency = 0}) == true);
        REQUIRE(c.client.s.submit_priority_update(first, {.urgency = 5}) == true);
        REQUIRE(c.client.s.submit_priority_update(third, {.urgency = 6}) == true);

        // (the third's head too.)
        std::vector<std::byte> head;
        c.client.s.take_output(head);
        CHECK(frames_with_updates(head)
              == std::vector<std::string>{"type 1", "update 3 [u=0]", "update 1 [u=5]", "update 5 [u=6]"});
        c.server.deliver(head);
        REQUIRE(c.client.s.submit_priority_update(second, {.urgency = 1}) == true);
        REQUIRE(c.client.s.submit_priority_update(first, {.urgency = 2}) == true);
        REQUIRE(c.client.s.submit_priority_update(third, {.urgency = 4}) == true);

        // the first's request goes on (its stream's half-closed (remote)); the second's has ended (so it's closed).
        REQUIRE_FALSE(c.server.s.submit_headers(first, response(), true));
        REQUIRE_FALSE(c.server.s.submit_headers(second, response(), true));
        std::vector<std::byte> answer;
        c.server.s.take_output(answer);
        c.client.deliver(answer);
        c.client.s.reset(third, http2::error_code::cancel);

        c.client.s.take_output(out);
        CHECK(frames_with_updates(out) == std::vector<std::string>{"type 3"});
    }
}

TEST_CASE("a client's PRIORITY_UPDATE is for a stream whose response may still come",
          "[http][http2][session][priority][client]")
{
    connected              c;
    std::vector<std::byte> out;
    const auto             ended    = open_request(c.client, request());
    const auto             sending  = open_request(c.client, request("POST"), false);
    const auto             answered = open_request(c.client, request());
    const auto             reset    = open_request(c.client, request());
    const auto             early    = open_request(c.client, request("POST"), false);
    c.client.s.take_output(out);
    c.server.deliver(out);

    // the server's ended one's response, and another's whose request is still being sent (early: half-closed (remote)),
    // and answered another without ending it; the client's reset one, and another's request is still being sent.
    REQUIRE_FALSE(c.server.s.submit_headers(ended, response(), true));
    REQUIRE_FALSE(c.server.s.submit_headers(answered, response(), false));
    REQUIRE_FALSE(c.server.s.submit_headers(early, response(), true));
    c.server.s.take_output(out);
    c.client.deliver(out);
    c.client.s.reset(reset, http2::error_code::cancel);
    c.client.s.take_output(out);

    // a stream whose request has ended (half-closed (local)), or is still being sent, may be reprioritized; one whose
    // response has ended (even with its request still being sent), one reset, and one the client never opened, can't.
    CHECK(c.client.s.submit_priority_update(answered, {.urgency = 1}) == true);
    CHECK(c.client.s.submit_priority_update(sending, {.urgency = 2}) == true);
    CHECK(c.client.s.submit_priority_update(ended, {.urgency = 3}) == false);
    CHECK(c.client.s.submit_priority_update(reset, {.urgency = 4}) == false);
    CHECK(c.client.s.submit_priority_update(early, {.urgency = 4}) == false);
    CHECK(c.client.s.submit_priority_update(99, {.urgency = 5}) == false);

    c.client.s.take_output(out);
    CHECK(updates_in(out) == std::vector<std::string>{"update 5 [u=1]", "update 3 [u=2]"});

    // nor once the connection's failed.
    c.client.deliver(std::vector<std::byte>(9, std::byte{0xff}));
    REQUIRE(c.client.failure);
    CHECK(c.client.s.submit_priority_update(answered, {.urgency = 6}) == false);
}

TEST_CASE("a server's session doesn't send PRIORITY_UPDATE", "[http][http2][session][priority]")
{
    connected  c;
    const auto id = open_request(c.client, request());
    pump(c.client, c.server);

    CHECK(c.server.s.submit_priority_update(id, {.urgency = 1}).error() == std::errc::invalid_argument);
    CHECK_FALSE(c.server.s.has_output());
}

namespace
{

// origin_entries is an ORIGIN frame's payload: each entry's length (16 bits) and origin.
std::vector<std::byte> origin_entries(std::initializer_list<std::string_view> origins)
{
    std::vector<std::byte> out;
    for (const auto o : origins)
    {
        out.push_back(static_cast<std::byte>(o.size() >> 8U));
        out.push_back(static_cast<std::byte>(o.size()));
        const auto bytes = bytes_of(o);
        out.insert(out.end(), bytes.begin(), bytes.end());
    }
    return out;
}

// origins_in lists out's ORIGIN frames, as "stream flags: [origin] ...", each entry's origin by its length (that many
// bytes, or what's left).
std::vector<std::string> origins_in(std::span<const std::byte> out)
{
    std::vector<std::string> found;
    while (out.size() >= 9)
    {
        const auto h       = http2::decode_frame_header(out.first<9>());
        auto       payload = out.subspan(9, h.length);
        out                = out.subspan(9 + h.length);
        if (h.type != static_cast<std::uint8_t>(http2::frame_type::origin)) continue;

        auto line = std::format("{} {}:", h.stream, h.flags);
        while (payload.size() >= 2)
        {
            const auto length = std::min<std::size_t>((std::to_integer<std::size_t>(payload[0]) << 8U)
                                                          | std::to_integer<std::size_t>(payload[1]),
                                                      payload.size() - 2);
            line += std::format(" [{}]", as_string(payload.subspan(2, length)));
            payload = payload.subspan(2 + length);
        }
        found.push_back(std::move(line));
    }
    return found;
}

// server_start is a server's empty SETTINGS, as a client's session receives first.
raw server_start()
{
    raw r;
    http2::encode_settings({}, {}, r.bytes);
    return r;
}

}

TEST_CASE("a server's ORIGIN lists its origins on stream 0, which the client reports", "[http][http2][session][origin]")
{
    connected c;

    const std::vector<std::string> origins{"https://a.example", "https://b.example:8443"};
    REQUIRE_FALSE(c.server.s.submit_origin(origins));

    std::vector<std::byte> out;
    c.server.s.take_output(out);
    CHECK(to_hex(out)
          == "00002b0c0000000000" // 43 bytes, ORIGIN, no flags, stream 0
             "0011"
                 + to_hex(bytes_of("https://a.example")) + "0016" + to_hex(bytes_of("https://b.example:8443")));

    c.client.deliver(out);
    REQUIRE_FALSE(c.client.failure);
    CHECK(c.client.events.take() == std::vector<std::string>{"origin [https://a.example] [https://b.example:8443]"});

    // none is a frame without entries: it still says the connection's only for its own origin.
    REQUIRE_FALSE(c.server.s.submit_origin({}));
    c.server.s.take_output(out);
    CHECK(origins_in(out) == std::vector<std::string>{"0 0:"});
    c.client.deliver(out);
    CHECK(c.client.events.take() == std::vector<std::string>{"origin"});
}

TEST_CASE("a server's ORIGIN is split into frames of the peer's MAX_FRAME_SIZE", "[http][http2][session][origin]")
{
    SECTION("the default, 16384")
    {
        connected c;

        // 2 bytes and 6000 an entry: two to a frame.
        const std::string              long_host(5'990, 'a');
        const std::vector<std::string> origins{"https://1" + long_host,
                                               "https://2" + long_host,
                                               "https://3" + long_host,
                                               "https://4" + long_host,
                                               "https://5" + long_host};
        REQUIRE_FALSE(c.server.s.submit_origin(origins));

        std::vector<std::byte> out;
        c.server.s.take_output(out);
        const auto frames = origins_in(out);
        REQUIRE(frames.size() == 3);
        CHECK(frames[0] == std::format("0 0: [{}] [{}]", origins[0], origins[1]));
        CHECK(frames[1] == std::format("0 0: [{}] [{}]", origins[2], origins[3]));
        CHECK(frames[2] == std::format("0 0: [{}]", origins[4]));

        c.client.deliver(out);
        REQUIRE_FALSE(c.client.failure);
        CHECK(c.client.events.take().size() == 3);

        // one that fills a frame by itself (16382 bytes and its length) fits; one more byte doesn't, and nothing's
        // sent.
        const std::vector<std::string> fills{std::string(16'382, 'x')};
        REQUIRE_FALSE(c.server.s.submit_origin(fills));
        c.server.s.take_output(out);
        CHECK(out.size() == 9 + 16'384);

        const std::vector<std::string> too_long{std::string(16'383, 'x')};
        CHECK(c.server.s.submit_origin(too_long) == std::errc::invalid_argument);
        CHECK_FALSE(c.server.s.has_output());
    }

    SECTION("a larger one, up to an entry's 65535 bytes")
    {
        http2::session_config client_config;
        client_config.local.max_frame_size = 1U << 20U;
        connected c{{}, client_config};

        const std::vector<std::string> larger{std::string(16'383, 'x'), std::string(65'535, 'y')};
        REQUIRE_FALSE(c.server.s.submit_origin(larger));
        std::vector<std::byte> out;
        c.server.s.take_output(out);
        CHECK(out.size() == 9 + 2 + 16'383 + 2 + 65'535);

        const std::vector<std::string> too_long{std::string(65'536, 'x')};
        CHECK(c.server.s.submit_origin(too_long) == std::errc::invalid_argument);
        CHECK_FALSE(c.server.s.has_output());
    }
}

TEST_CASE("a client's session doesn't send ORIGIN", "[http][http2][session][origin]")
{
    connected                      c;
    const std::vector<std::string> origins{"https://a.example"};
    CHECK(c.client.s.submit_origin(origins) == std::errc::invalid_argument);
    CHECK_FALSE(c.client.s.has_output());
}

TEST_CASE("a server's session ignores ORIGIN", "[http][http2][session][origin]")
{
    endpoint server{http2::role::server};

    raw r;
    r.client_start()
        .frame(http2::frame_type::origin, 0, 0, origin_entries({"https://a.example"}))
        .frame(http2::frame_type::origin, 0, 0, origin_entries({}))
        .frame(http2::frame_type::origin, 0, 1, origin_entries({"https://a.example"}))
        .frame(http2::frame_type::origin, 0, 0, std::vector<std::byte>{std::byte{0xff}})
        .headers(1, request(), end_both);
    server.deliver(r.bytes);
    REQUIRE_FALSE(server.failure);
    CHECK(server.events.take()
          == std::vector<std::string>{"settings",
                                      "headers 1 head end :method=GET :scheme=https :authority=example.com :path=/"});

    // nothing in answer but the SETTINGS' acknowledgement.
    std::vector<std::byte> out;
    server.s.take_output(out);
    CHECK(frames_of(out, http2::frame_type::goaway).empty());
    CHECK(frames_of(out, http2::frame_type::rst_stream).empty());
}

TEST_CASE("a client ignores ORIGIN on a stream other than 0, or with a reserved flag", "[http][http2][session][origin]")
{
    endpoint client{http2::role::client};

    auto r = server_start();
    r.frame(http2::frame_type::origin, 0, 1, origin_entries({"https://on-stream.example"}));
    for (const std::uint8_t flag : {0x1, 0x2, 0x4, 0x8})
        r.frame(http2::frame_type::origin, flag, 0, origin_entries({"https://flagged.example"}));

    // flags past the four RFC 8336 reserves aren't its concern.
    r.frame(http2::frame_type::origin, 0xf0, 0, origin_entries({"https://a.example"}));
    client.deliver(r.bytes);
    REQUIRE_FALSE(client.failure);
    CHECK(client.events.take() == std::vector<std::string>{"settings", "origin [https://a.example]"});
}

TEST_CASE("a client reports ORIGIN's entries up to one that runs past the frame", "[http][http2][session][origin]")
{
    endpoint client{http2::role::client};

    // an entry claiming 100 bytes, with 3: it and the rest of the frame are ignored.
    auto past = origin_entries({"https://a.example", ""});
    past.insert(past.end(), {std::byte{0}, std::byte{100}, std::byte{'a'}, std::byte{'b'}, std::byte{'c'}});
    const auto after = origin_entries({"https://b.example"});
    past.insert(past.end(), after.begin(), after.end());

    // a byte left over, short of a length; and a frame whose only entry runs past it, reported without entries.
    auto odd = origin_entries({"https://c.example"});
    odd.push_back(std::byte{0});

    // one that runs past by a byte.
    auto just_past = origin_entries({"https://e.example"});
    just_past.insert(just_past.end(), {std::byte{0}, std::byte{3}, std::byte{'a'}, std::byte{'b'}});

    auto r = server_start();
    r.frame(http2::frame_type::origin, 0, 0, past)
        .frame(http2::frame_type::origin, 0, 0, just_past)
        .frame(http2::frame_type::origin, 0, 0, odd)
        .frame(http2::frame_type::origin, 0, 0, std::vector<std::byte>{std::byte{0xff}, std::byte{0xff}})
        .frame(http2::frame_type::origin, 0, 0, origin_entries({"not an origin", "https://d.example"}));
    client.deliver(r.bytes);
    REQUIRE_FALSE(client.failure);

    // entries that aren't origins are reported as they are: they're the application's to parse.
    CHECK(client.events.take()
          == std::vector<std::string>{
              "settings",
              "origin [https://a.example] []",
              "origin [https://e.example]",
              "origin [https://c.example]",
              "origin",
              "origin [not an origin] [https://d.example]",
          });
}

TEST_CASE("ORIGIN in the middle of a header block is a connection error, as any frame",
          "[http][http2][session][origin]")
{
    endpoint client{http2::role::client};
    REQUIRE(client.s.open_stream() == 1U);
    REQUIRE_FALSE(client.s.submit_headers(1, request(), true));

    hpack::encoder         enc;
    std::vector<std::byte> block;
    enc.encode(response(), block);

    auto r = server_start();
    r.frame(http2::frame_type::headers, 0, 1, block).frame(http2::frame_type::origin, 0, 0, origin_entries({}));
    client.deliver(r.bytes);
    CHECK(client.failure == http2::error_code::protocol_error);
}

namespace
{

// connect_protocol is a session_config whose server sends SETTINGS_ENABLE_CONNECT_PROTOCOL = 1.
http2::session_config connect_protocol()
{
    http2::session_config config;
    config.local.enable_connect_protocol = true;
    return config;
}

// extended_connect is an extended CONNECT (RFC 8441) for a WebSocket.
std::vector<hpack::field> extended_connect()
{
    return {
        {              .name = ":method",     .value = "CONNECT"},
        {            .name = ":protocol",   .value = "websocket"},
        {              .name = ":scheme",       .value = "https"},
        {           .name = ":authority", .value = "example.com"},
        {                .name = ":path",       .value = "/chat"},
        {.name = "sec-websocket-version",          .value = "13"},
    };
}

// plain_connect is RFC 9113's CONNECT, for a tunnel to a host and port.
std::vector<hpack::field> plain_connect()
{
    return {
        {   .name = ":method",         .value = "CONNECT"},
        {.name = ":authority", .value = "example.com:443"},
    };
}

// without is fields without those named name.
std::vector<hpack::field> without(std::vector<hpack::field> fields, std::string_view name)
{
    std::erase_if(fields, [&](const hpack::field& f) { return f.name == name; });
    return fields;
}

// with is fields with one more, after the others (a regular field) or at the pseudo-headers' end (a pseudo-header).
std::vector<hpack::field> with(std::vector<hpack::field> fields, hpack::field f)
{
    const auto at = f.name.starts_with(':')
                      ? std::ranges::find_if(fields, [](const hpack::field& g) { return !g.name.starts_with(':'); })
                      : fields.end();
    fields.insert(at, f);
    return fields;
}

// replaced is fields with name's value replaced.
std::vector<hpack::field> replaced(std::vector<hpack::field> fields, std::string_view name, std::string_view value)
{
    for (auto& f : fields)
    {
        if (f.name == name) f.value = value;
    }
    return fields;
}

}

TEST_CASE("a server sends SETTINGS_ENABLE_CONNECT_PROTOCOL as 1 if its config says so; a client never sends it",
          "[http][http2][session][connect-protocol]")
{
    const auto sends_it = [](const std::vector<std::string>& settings)
    { return std::ranges::any_of(settings, [](const std::string& p) { return p.starts_with("8="); }); };

    std::vector<std::byte> out;

    endpoint server{http2::role::server, connect_protocol()};
    server.s.take_output(out);
    CHECK(std::ranges::contains(settings_in(out), "8=1"));

    endpoint by_default{http2::role::server};
    by_default.s.take_output(out);
    CHECK_FALSE(sends_it(settings_in(out)));

    endpoint client{http2::role::client, connect_protocol()};
    client.s.take_output(out);
    CHECK_FALSE(sends_it(settings_in(out)));

    // each side reads the other's: the client has it, the server nothing.
    connected c{connect_protocol(), connect_protocol()};
    CHECK(c.client.s.peer_settings().enable_connect_protocol);
    CHECK_FALSE(c.server.s.peer_settings().enable_connect_protocol);
    CHECK_FALSE(c.client.failure);
    CHECK_FALSE(c.server.failure);
}

TEST_CASE("a client reads SETTINGS_ENABLE_CONNECT_PROTOCOL: 0 or 1, and never withdrawn once 1",
          "[http][http2][session][connect-protocol]")
{
    struct test_case
    {
        std::string_view                        what;
        std::vector<std::vector<std::uint32_t>> frames; // each SETTINGS' values of the setting, the first first
        bool                                    fails;
        bool                                    value = false;
    };

    const auto tc = GENERATE(
        test_case{
            .what   = "1",
            .frames = {{1}},
            .fails  = false,
            .value  = true
    },
        test_case{.what = "0", .frames = {{0}}, .fails = false},
        test_case{.what = "left out", .frames = {{}}, .fails = false},
        test_case{.what = "2", .frames = {{2}}, .fails = true},
        test_case{.what = "2^32-1", .frames = {{0xffff'ffff}}, .fails = true},
        test_case{.what = "1, then 1 again", .frames = {{1}, {1}}, .fails = false, .value = true},
        test_case{.what = "1, then left out", .frames = {{1}, {}}, .fails = false, .value = true},
        test_case{.what = "1, then 0", .frames = {{1}, {0}}, .fails = true},
        test_case{.what = "1 then 0 in one SETTINGS", .frames = {{1, 0}}, .fails = true},
        test_case{.what = "0 then 1 in one SETTINGS", .frames = {{0, 1}}, .fails = false, .value = true},
        test_case{.what = "left out, then 1", .frames = {{}, {1}}, .fails = false, .value = true},
        test_case{.what = "0, then 1", .frames = {{0}, {1}}, .fails = false, .value = true},
        test_case{.what = "1, then 2", .frames = {{1}, {2}}, .fails = true});
    CAPTURE(tc.what);

    raw r;
    for (const auto& values : tc.frames)
    {
        std::vector<std::byte> payload;
        for (const auto v : values)
        {
            const auto one = setting(http2::setting_id::enable_connect_protocol, v);
            payload.insert(payload.end(), one.begin(), one.end());
        }
        r.frame(http2::frame_type::settings, 0, 0, payload);
    }

    endpoint client{http2::role::client};
    client.deliver(r.bytes);
    if (tc.fails)
    {
        CHECK(client.failure == http2::error_code::protocol_error);
        return;
    }

    REQUIRE_FALSE(client.failure);
    CHECK(client.s.peer_settings().enable_connect_protocol == tc.value);
}

TEST_CASE("a server ignores a client's SETTINGS_ENABLE_CONNECT_PROTOCOL, but for a value past 1",
          "[http][http2][session][connect-protocol]")
{
    struct test_case
    {
        std::string_view                        what;
        std::vector<std::vector<std::uint32_t>> frames;
        bool                                    fails = false;
    };

    const auto tc = GENERATE(
        test_case{
            .what   = "1",
            .frames = {{1}}
    },
        test_case{.what = "1, then 0", .frames = {{1}, {0}}},
        test_case{.what = "2", .frames = {{2}}, .fails = true});
    CAPTURE(tc.what);

    const bool takes = GENERATE(false, true);
    CAPTURE(takes);

    raw        r;
    const auto p = bytes_of(http2::preface);
    r.bytes.insert(r.bytes.end(), p.begin(), p.end());
    for (const auto& values : tc.frames)
    {
        std::vector<std::byte> payload;
        for (const auto v : values)
        {
            const auto one = setting(http2::setting_id::enable_connect_protocol, v);
            payload.insert(payload.end(), one.begin(), one.end());
        }
        r.frame(http2::frame_type::settings, 0, 0, payload);
    }

    endpoint server{http2::role::server, takes ? connect_protocol() : http2::session_config{}};
    server.deliver(r.bytes);
    if (tc.fails)
    {
        CHECK(server.failure == http2::error_code::protocol_error);
        return;
    }

    REQUIRE_FALSE(server.failure);
    CHECK_FALSE(server.s.peer_settings().enable_connect_protocol);
}

TEST_CASE("a server that sent ENABLE_CONNECT_PROTOCOL takes extended CONNECT, reporting :protocol; DATA goes both ways",
          "[http][http2][session][connect-protocol]")
{
    connected c{connect_protocol()};
    REQUIRE(c.client.s.peer_settings().enable_connect_protocol);

    const auto id = c.client.s.open_stream().value();
    REQUIRE_FALSE(c.client.s.submit_headers(id, extended_connect(), false));
    pump(c.client, c.server);
    CHECK(c.server.events.take()
          == std::vector<std::string>{"headers 1 head :method=CONNECT :protocol=websocket :scheme=https "
                                      ":authority=example.com :path=/chat sec-websocket-version=13"});

    REQUIRE_FALSE(c.server.s.submit_headers(id, response(), false));
    REQUIRE(c.server.s.submit_data(id, bytes_of("from the server"), false) == 15U);
    REQUIRE(c.client.s.submit_data(id, bytes_of("from the client"), false) == 15U);
    pump(c.client, c.server);
    CHECK(c.client.events.take() == std::vector<std::string>{"headers 1 head :status=200", "data 1 [from the server]"});
    CHECK(c.server.events.take() == std::vector<std::string>{"data 1 [from the client]"});

    // each side's END_STREAM is its orderly close (RFC 8441, section 5).
    REQUIRE(c.client.s.submit_data(id, {}, true) == 0U);
    REQUIRE(c.server.s.submit_data(id, {}, true) == 0U);
    pump(c.client, c.server);
    CHECK(c.client.events.take() == std::vector<std::string>{"data 1 [] end"});
    CHECK(c.server.events.take() == std::vector<std::string>{"data 1 [] end"});
    CHECK(c.client.s.open_streams() == 0);
    CHECK(c.server.s.open_streams() == 0);
}

TEST_CASE("extended CONNECT is malformed without the setting, on another method, or without what it needs",
          "[http][http2][session][connect-protocol][malformed]")
{
    struct test_case
    {
        std::string_view          what;
        std::vector<hpack::field> fields;
        bool                      takes = true; // the server sent ENABLE_CONNECT_PROTOCOL = 1
    };

    // :protocol after a regular field.
    auto after_field = without(extended_connect(), ":protocol");
    after_field.push_back({.name = ":protocol", .value = "websocket"});

    const auto tc =
        GENERATE_REF(test_case{.what = "without the setting", .fields = extended_connect(), .takes = false},
                     test_case{.what = "on GET", .fields = replaced(extended_connect(), ":method", "GET")},
                     test_case{.what = "on POST", .fields = replaced(extended_connect(), ":method", "POST")},
                     test_case{.what = "no :scheme", .fields = without(extended_connect(), ":scheme")},
                     test_case{.what = "no :path", .fields = without(extended_connect(), ":path")},
                     test_case{.what = "an empty :path", .fields = replaced(extended_connect(), ":path", "")},
                     test_case{.what = "no :authority", .fields = without(extended_connect(), ":authority")},
                     test_case{.what = "an empty :protocol", .fields = replaced(extended_connect(), ":protocol", "")},
                     test_case{.what   = "a :protocol that isn't a token",
                               .fields = replaced(extended_connect(), ":protocol", "web socket")},
                     test_case{.what   = "two :protocols",
                               .fields = with(extended_connect(), {.name = ":protocol", .value = "websocket"})},
                     test_case{.what = ":protocol after a field", .fields = after_field},
                     test_case{.what   = "Host disagreeing with :authority",
                               .fields = with(extended_connect(), {.name = "host", .value = "other.example"})},
                     test_case{.what   = "a connection-specific field",
                               .fields = with(extended_connect(), {.name = "upgrade", .value = "websocket"})});
    CAPTURE(tc.what);

    endpoint server{http2::role::server, tc.takes ? connect_protocol() : http2::session_config{}};
    raw      r;
    r.client_start().headers(1, tc.fields, http2::flags::end_headers);
    server.deliver(r.bytes);
    REQUIRE_FALSE(server.failure);
    CHECK(server.events.take() == std::vector<std::string>{"settings"});

    std::vector<std::byte> out;
    server.s.take_output(out);
    CHECK(frames_of(out, http2::frame_type::rst_stream) == std::vector<std::string>{"1 1"});

    // the same, but valid, still works after it.
    r.bytes.clear();
    r.headers(3, extended_connect(), http2::flags::end_headers);
    server.deliver(r.bytes);
    REQUIRE_FALSE(server.failure);
    CHECK(server.events.take().size() == (tc.takes ? 1U : 0U));
}

TEST_CASE("plain CONNECT keeps RFC 9113's rules, whether or not the server takes extended CONNECT",
          "[http][http2][session][connect-protocol]")
{
    struct test_case
    {
        std::string_view          what;
        std::vector<hpack::field> fields;
        bool                      valid = false;
    };

    const auto tc = GENERATE(
        test_case{.what = "only :method and :authority", .fields = plain_connect(), .valid = true},
        test_case{.what = "with :scheme", .fields = with(plain_connect(), {.name = ":scheme", .value = "https"})},
        test_case{.what = "with :path", .fields = with(plain_connect(), {.name = ":path", .value = "/"})},
        test_case{.what = "no :authority", .fields = without(plain_connect(), ":authority")});
    CAPTURE(tc.what);

    const bool takes = GENERATE(false, true);
    CAPTURE(takes);

    endpoint server{http2::role::server, takes ? connect_protocol() : http2::session_config{}};
    raw      r;
    r.client_start().headers(1, tc.fields, http2::flags::end_headers);
    server.deliver(r.bytes);
    REQUIRE_FALSE(server.failure);

    std::vector<std::byte> out;
    server.s.take_output(out);
    if (tc.valid)
    {
        CHECK(server.events.take()
              == std::vector<std::string>{"settings", "headers 1 head :method=CONNECT :authority=example.com:443"});
        CHECK(frames_of(out, http2::frame_type::rst_stream).empty());
    }
    else
    {
        CHECK(server.events.take() == std::vector<std::string>{"settings"});
        CHECK(frames_of(out, http2::frame_type::rst_stream) == std::vector<std::string>{"1 1"});
    }
}

TEST_CASE("a server's CONNECT stream is a tunnel: a Content-Length doesn't hold its DATA, and no trailers follow",
          "[http][http2][session][connect-protocol]")
{
    const auto extended = GENERATE(false, true);
    CAPTURE(extended);

    const auto connect = extended ? extended_connect() : plain_connect();
    endpoint   server{http2::role::server, connect_protocol()};

    SECTION("DATA past a Content-Length, and ending short of another")
    {
        raw r;
        r.client_start()
            .headers(1, with(connect, {.name = "content-length", .value = "3"}), http2::flags::end_headers)
            .frame(http2::frame_type::data, 0, 1, bytes_of("longer than three"))
            .headers(3, with(connect, {.name = "content-length", .value = "100"}), http2::flags::end_headers)
            .frame(http2::frame_type::data, http2::flags::end_stream, 3, bytes_of("short"));
        server.deliver(r.bytes);
        REQUIRE_FALSE(server.failure);

        const auto events = server.events.take();
        REQUIRE(events.size() == 5);
        CHECK(events[2] == "data 1 [longer than three]");
        CHECK(events[4] == "data 3 [short] end");

        std::vector<std::byte> out;
        server.s.take_output(out);
        CHECK(frames_of(out, http2::frame_type::rst_stream).empty());
    }

    SECTION("a head that ends the stream, with a Content-Length")
    {
        raw r;
        r.client_start().headers(1, with(connect, {.name = "content-length", .value = "5"}), end_both);
        server.deliver(r.bytes);
        REQUIRE_FALSE(server.failure);
        CHECK(server.events.take().size() == 2);
    }

    SECTION("trailers are the stream's error")
    {
        const std::vector<hpack::field> trailers{
            {.name = "x", .value = "y"}
        };

        raw r;
        r.client_start()
            .headers(1, connect, http2::flags::end_headers)
            .frame(http2::frame_type::data, 0, 1, bytes_of("data"))
            .headers(1, trailers, end_both);
        server.deliver(r.bytes);
        REQUIRE_FALSE(server.failure);

        const auto events = server.events.take();
        REQUIRE(events.size() == 4);
        CHECK(events[2] == "data 1 [data]");
        CHECK(events[3] == "reset 1 protocol error");

        std::vector<std::byte> out;
        server.s.take_output(out);
        CHECK(frames_of(out, http2::frame_type::rst_stream) == std::vector<std::string>{"1 1"});
    }

    SECTION("a request that isn't CONNECT keeps to its Content-Length, and may have trailers")
    {
        const std::vector<hpack::field> trailers{
            {.name = "x", .value = "y"}
        };

        raw r;
        r.client_start()
            .headers(1, with(request("POST"), {.name = "content-length", .value = "4"}), http2::flags::end_headers)
            .frame(http2::frame_type::data, 0, 1, bytes_of("data"))
            .headers(1, trailers, end_both)
            .headers(3, with(request("POST"), {.name = "content-length", .value = "3"}), http2::flags::end_headers)
            .frame(http2::frame_type::data, 0, 3, bytes_of("data"));
        server.deliver(r.bytes);
        REQUIRE_FALSE(server.failure);

        const auto events = server.events.take();
        REQUIRE(events.size() == 6);
        CHECK(events[3] == "headers 1 trailers end x=y");
        CHECK(events[4].starts_with("headers 3 head"));
        CHECK(events[5] == "reset 3 protocol error");

        std::vector<std::byte> out;
        server.s.take_output(out);
        CHECK(frames_of(out, http2::frame_type::rst_stream) == std::vector<std::string>{"3 1"});
    }
}

TEST_CASE("a client sends extended CONNECT only once the server has sent ENABLE_CONNECT_PROTOCOL = 1",
          "[http][http2][session][connect-protocol]")
{
    endpoint client{http2::role::client};

    // what the client sends, for a server to receive at the end.
    std::vector<std::byte> sent;
    const auto             take = [&]
    {
        std::vector<std::byte> out;
        client.s.take_output(out);
        sent.insert(sent.end(), out.begin(), out.end());
    };
    take();

    const auto refused = [&](std::string_view when)
    {
        CAPTURE(when);
        const auto id = client.s.open_stream().value();
        CHECK(client.s.submit_headers(id, extended_connect(), false) == std::errc::invalid_argument);

        // nothing's sent, nor kept: not even a reset, which the server would take as a connection error (the stream's
        // idle to it).
        client.s.reset(id, http2::error_code::cancel);
        CHECK_FALSE(client.s.has_output());
        CHECK(client.s.open_streams() == 1);
        CHECK(client.s.submit_data(id, bytes_of("x"), true).error() == http2::error_code::stream_closed);
        return id;
    };

    // a plain CONNECT needs no setting.
    const auto plain = client.s.open_stream().value();
    CHECK(plain == 1);
    REQUIRE_FALSE(client.s.submit_headers(plain, plain_connect(), false));
    take();

    CHECK(refused("before the server's SETTINGS") == 3);

    // the server's SETTINGS without it, then with 0.
    auto r = server_start();
    client.deliver(r.bytes);
    take();
    CHECK(refused("without it") == 5);

    r.bytes.clear();
    r.frame(http2::frame_type::settings, 0, 0, setting(http2::setting_id::enable_connect_protocol, 0));
    client.deliver(r.bytes);
    take();
    CHECK(refused("with 0") == 7);

    // then 1.
    r.bytes.clear();
    r.frame(http2::frame_type::settings, 0, 0, setting(http2::setting_id::enable_connect_protocol, 1));
    client.deliver(r.bytes);
    REQUIRE_FALSE(client.failure);
    take();

    const auto id = client.s.open_stream().value();
    CHECK(id == 9);
    REQUIRE_FALSE(client.s.submit_headers(id, extended_connect(), false));
    CHECK(client.s.open_streams() == 2);
    take();

    // which a server that takes it receives, the stream ids skipped being closed to it (RFC 9113, section 5.1.1).
    endpoint server{http2::role::server, connect_protocol()};
    server.deliver(sent);
    REQUIRE_FALSE(server.failure);
    CHECK(server.events.take()
          == std::vector<std::string>{"settings",
                                      "headers 1 head :method=CONNECT :authority=example.com:443",
                                      "headers 9 head :method=CONNECT :protocol=websocket :scheme=https "
                                      ":authority=example.com :path=/chat sec-websocket-version=13"});
    std::vector<std::byte> out;
    server.s.take_output(out);
    CHECK(frames_of(out, http2::frame_type::rst_stream).empty());
    CHECK(frames_of(out, http2::frame_type::goaway).empty());
}

TEST_CASE("a client's CONNECT is a tunnel once its response is a 2xx: its Content-Length is ignored, and no trailers "
          "follow",
          "[http][http2][session][connect-protocol]")
{
    const auto extended = GENERATE(false, true);
    CAPTURE(extended);

    connected  c{connect_protocol()};
    const auto id = c.client.s.open_stream().value();
    REQUIRE_FALSE(c.client.s.submit_headers(id, extended ? extended_connect() : plain_connect(), false));
    pump(c.client, c.server);
    c.server.events.take();

    const std::vector<hpack::field> trailers{
        {.name = "x", .value = "y"}
    };

    SECTION("a 2xx's Content-Length")
    {
        const auto status = GENERATE("200"sv, "299"sv);
        REQUIRE_FALSE(
            c.server.s.submit_headers(id, with(response(status), {.name = "content-length", .value = "0"}), false));
        REQUIRE(c.server.s.submit_data(id, bytes_of("hello"), false) == 5U);
        pump(c.client, c.server);
        CHECK(c.client.events.take()
              == std::vector<std::string>{std::format("headers 1 head :status={} content-length=0", status),
                                          "data 1 [hello]"});

        REQUIRE(c.server.s.submit_data(id, {}, true) == 0U);
        pump(c.client, c.server);
        CHECK(c.client.events.take() == std::vector<std::string>{"data 1 [] end"});
    }

    SECTION("trailers after a 2xx, after an informational response")
    {
        REQUIRE_FALSE(c.server.s.submit_headers(id, response("103"), false));
        REQUIRE_FALSE(c.server.s.submit_headers(id, response(), false));
        REQUIRE_FALSE(c.server.s.submit_headers(id, trailers, true));
        pump(c.client, c.server);
        CHECK(c.client.events.take()
              == std::vector<std::string>{"headers 1 informational :status=103",
                                          "headers 1 head :status=200",
                                          "reset 1 protocol error"});
    }

    SECTION("a refusal is a response as any other: its Content-Length holds, and trailers may follow")
    {
        REQUIRE_FALSE(
            c.server.s.submit_headers(id, with(response("404"), {.name = "content-length", .value = "3"}), false));
        REQUIRE(c.server.s.submit_data(id, bytes_of("abc"), false) == 3U);
        REQUIRE_FALSE(c.server.s.submit_headers(id, trailers, true));
        pump(c.client, c.server);
        CHECK(c.client.events.take()
              == std::vector<std::string>{"headers 1 head :status=404 content-length=3",
                                          "data 1 [abc]",
                                          "headers 1 trailers end x=y"});
    }

    SECTION("a refusal's DATA past its Content-Length")
    {
        REQUIRE_FALSE(
            c.server.s.submit_headers(id, with(response("404"), {.name = "content-length", .value = "3"}), false));
        REQUIRE(c.server.s.submit_data(id, bytes_of("too long"), false) == 8U);
        pump(c.client, c.server);
        CHECK(c.client.events.take()
              == std::vector<std::string>{"headers 1 head :status=404 content-length=3", "reset 1 protocol error"});
    }
}
