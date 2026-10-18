#include "http/http2.hpp"

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
#include "http/http2_session.hpp"

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

// goaway_code is the error code of the GOAWAY frame in out, or nullopt if there's none.
std::optional<std::uint32_t> goaway_code(std::span<const std::byte> out)
{
    // a server's output starts with frames; a client's with the preface.
    if (as_string(out).starts_with(http2::preface)) out = out.subspan(http2::preface.size());

    while (out.size() >= 9)
    {
        const auto h       = http2::decode_frame_header(out.first<9>());
        const auto payload = out.subspan(9, h.length);
        if (h.type == static_cast<std::uint8_t>(http2::frame_type::goaway))
        {
            return (std::to_integer<std::uint32_t>(payload[4]) << 24U)
                 | (std::to_integer<std::uint32_t>(payload[5]) << 16U)
                 | (std::to_integer<std::uint32_t>(payload[6]) << 8U) | std::to_integer<std::uint32_t>(payload[7]);
        }
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
                  .fields = {fields[0], fields[1], fields[2], fields[3], {.name = ":protocol", .value = "websocket"}}},
        test_case{.what   = "a value with a newline",
                  .fields = {fields[0], fields[1], fields[2], fields[3], {.name = "x", .value = "a\nb"}}},
        test_case{.what   = "a value with leading space",
                  .fields = {fields[0], fields[1], fields[2], fields[3], {.name = "x", .value = " a"}}},
        test_case{.what   = "Host disagreeing with :authority",
                  .fields = {fields[0], fields[1], fields[2], fields[3], {.name = "host", .value = "other.com"}}},
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
