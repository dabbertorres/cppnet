#include "websocket/session.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "websocket/error.hpp"
#include "websocket/frame.hpp"
#include "websocket/frames.hpp"

namespace
{

using namespace std::string_view_literals;

using net::websocket::close_code;
using net::websocket::sent;
using test::websocket::bytes;
using test::websocket::join;
using test::websocket::unhex;

namespace ws = net::websocket::detail;

using kind = ws::received::kind;

constexpr std::array<std::byte, 4> key{std::byte{0x37}, std::byte{0xfa}, std::byte{0x21}, std::byte{0x3d}};

// to_server is a frame a client sends (masked); to_client one a server sends (not).
std::vector<std::byte> to_server(ws::opcode op, bool fin, std::span<const std::byte> payload, std::uint8_t rsv = 0)
{
    return test::websocket::frame({.fin = fin, .rsv = rsv, .op = op, .masked = true, .mask_key = key}, payload);
}

std::vector<std::byte> to_client(ws::opcode op, bool fin, std::span<const std::byte> payload, std::uint8_t rsv = 0)
{
    return test::websocket::frame({.fin = fin, .rsv = rsv, .op = op}, payload);
}

std::vector<std::byte> to(ws::role r, ws::opcode op, bool fin, std::span<const std::byte> payload, std::uint8_t rsv = 0)
{
    return r == ws::role::server ? to_server(op, fin, payload, rsv) : to_client(op, fin, payload, rsv);
}

// read reads input through a new session of role r, at once and a byte at a time, and checks that the two agree: the
// same frames, and the same failure, at the same byte.
test::websocket::trace read(ws::role r, std::span<const std::byte> input, const ws::limits& limits = {})
{
    ws::session whole_session{r, limits};
    ws::session bytewise_session{r, limits};
    auto        whole    = test::websocket::read(whole_session, input, std::numeric_limits<std::size_t>::max());
    const auto  bytewise = test::websocket::read(bytewise_session, input, 1);

    CHECK(whole.events == bytewise.events);
    CHECK(whole.failure == bytewise.failure);
    CHECK(whole.used == bytewise.used);
    if (whole.failure)
    {
        CHECK(whole.failed_from <= bytewise.failed_byte());
        CHECK(bytewise.failed_byte() < whole.failed_to);
    }
    return whole;
}

// error_of is what result failed with; none if it didn't (so a check of it fails, rather than reading an error that
// isn't there).
template<typename T>
std::error_code error_of(const std::expected<T, std::error_code>& result)
{
    return result ? std::error_code{} : result.error();
}

test::websocket::event message(ws::opcode type, std::span<const std::byte> data)
{
    return {
        .what = kind::message_end,
        .type = type,
        .data = {data.begin(), data.end()}
    };
}

}

TEST_CASE("sessions read RFC 6455's example frames", "[websocket][session]")
{
    SECTION("a client's masked text message, to a server")
    {
        const auto out = read(ws::role::server, unhex("81 85 37 fa 21 3d 7f 9f 4d 51 58"));
        CHECK_FALSE(out.failure);
        CHECK(out.events == std::vector{message(ws::opcode::text, bytes("Hello"))});
    }

    SECTION("a server's unmasked text message, fragmented, to a client")
    {
        const auto out = read(ws::role::client, unhex("01 03 48 65 6c 80 02 6c 6f"));
        CHECK_FALSE(out.failure);
        CHECK(out.events
              == std::vector<test::websocket::event>{
                  {.what = kind::data, .type = ws::opcode::text},
                  message(ws::opcode::text, bytes("Hello"))
        });
    }

    SECTION("an unmasked ping, to a client, and a masked pong, to a server")
    {
        const auto ping = read(ws::role::client, unhex("89 05 48 65 6c 6c 6f"));
        CHECK(ping.events
              == std::vector<test::websocket::event>{
                  {.what = kind::ping, .type = ws::opcode::ping, .data = bytes("Hello")}
        });

        const auto pong = read(ws::role::server, unhex("8a 85 37 fa 21 3d 7f 9f 4d 51 58"));
        CHECK(pong.events
              == std::vector<test::websocket::event>{
                  {.what = kind::pong, .type = ws::opcode::pong, .data = bytes("Hello")}
        });
    }

    SECTION("256 bytes and 64 KiB of binary")
    {
        for (const std::size_t size : {std::size_t{256}, std::size_t{65536}})
        {
            CAPTURE(size);
            std::vector<std::byte> data(size);
            for (std::size_t i = 0; i < size; ++i) data[i] = static_cast<std::byte>(i % 251);

            const auto client = read(ws::role::client, to_client(ws::opcode::binary, true, data));
            CHECK(client.events == std::vector{message(ws::opcode::binary, data)});
            const auto server = read(ws::role::server, to_server(ws::opcode::binary, true, data));
            CHECK(server.events == std::vector{message(ws::opcode::binary, data)});
        }
    }
}

TEST_CASE("sessions fail the connection for frames the rules refuse", "[websocket][session]")
{
    const auto        protocol_error  = sent(close_code::protocol_error);
    const auto        invalid_payload = sent(close_code::invalid_payload);
    const auto        message_too_big = sent(close_code::message_too_big);
    const auto        r               = GENERATE(ws::role::server, ws::role::client);
    const auto        other           = r == ws::role::server ? ws::role::client : ws::role::server;
    const std::size_t head            = r == ws::role::server ? 6 : 2; // a short frame's header
    const auto        hello           = bytes("Hello");
    const auto        text = [&](bool fin, std::string_view data) { return to(r, ws::opcode::text, fin, bytes(data)); };
    const auto        more = [&](bool fin, std::string_view data)
    { return to(r, ws::opcode::continuation, fin, bytes(data)); };

    struct test_case
    {
        std::string_view       name;
        std::vector<std::byte> input;
        std::error_code        failure{};
        std::size_t            frames    = 0; // taken before the failure (or at all)
        std::uint64_t          max       = std::uint64_t{1} << 20;
        std::size_t            failed_at = 0; // the byte that fails, if it's not the last frame's last (a header's)
    };

    auto tc = GENERATE_COPY(
        // masking per role
        test_case{.name    = "masked as the other role masks",
                  .input   = to(other, ws::opcode::text, true, hello),
                  .failure = protocol_error},
        // lengths, which fail as soon as they're read (before a mask key)
        test_case{.name      = "a length not in its minimal form",
                  .input     = join(unhex(r == ws::role::server ? "81 fe 00 05" : "81 7e 00 05"), bytes("Hello")),
                  .failure   = protocol_error,
                  .failed_at = 3},
        test_case{.name = "a 64-bit length with its top bit set",
                  .input =
                      unhex(r == ws::role::server ? "82 ff 80 00 00 00 00 01 00 00" : "82 7f 80 00 00 00 00 01 00 00"),
                  .failure   = protocol_error,
                  .failed_at = 9},
        // reserved bits, on data and control frames
        test_case{.name = "RSV1", .input = to(r, ws::opcode::text, true, hello, 4), .failure = protocol_error},
        test_case{.name = "RSV2", .input = to(r, ws::opcode::binary, true, hello, 2), .failure = protocol_error},
        test_case{.name    = "RSV3 on a ping",
                  .input   = to(r, ws::opcode::ping, true, hello, 1),
                  .failure = protocol_error},
        // reserved opcodes
        test_case{.name = "opcode 3", .input = to(r, ws::opcode{3}, true, {}), .failure = protocol_error},
        test_case{.name = "opcode 7", .input = to(r, ws::opcode{7}, true, hello), .failure = protocol_error},
        test_case{.name = "opcode 0xb", .input = to(r, ws::opcode{0xb}, true, {}), .failure = protocol_error},
        test_case{.name = "opcode 0xf", .input = to(r, ws::opcode{0xf}, true, hello), .failure = protocol_error},
        // control frames
        test_case{.name    = "a fragmented ping",
                  .input   = to(r, ws::opcode::ping, false, hello),
                  .failure = protocol_error},
        test_case{.name    = "a fragmented close",
                  .input   = to(r, ws::opcode::close, false, unhex("03 e8")),
                  .failure = protocol_error},
        test_case{.name    = "a ping of 126 bytes",
                  .input   = to(r, ws::opcode::ping, true, std::vector<std::byte>(126)),
                  .failure = protocol_error},
        test_case{.name   = "a pong of 125 bytes",
                  .input  = to(r, ws::opcode::pong, true, std::vector<std::byte>(125)),
                  .frames = 1},
        // fragmentation
        test_case{.name = "a continuation without a message", .input = more(true, "lo"), .failure = protocol_error},
        test_case{.name    = "a continuation after a message",
                  .input   = join(text(true, "Hello"), more(true, "!")),
                  .failure = protocol_error,
                  .frames  = 1},
        test_case{.name    = "a text frame inside a message",
                  .input   = join(text(false, "Hel"), text(true, "lo")),
                  .failure = protocol_error,
                  .frames  = 1},
        test_case{.name    = "a binary frame inside a message",
                  .input   = join(text(false, "Hel"), to(r, ws::opcode::binary, true, hello)),
                  .failure = protocol_error,
                  .frames  = 1},
        test_case{.name   = "empty fragments",
                  .input  = join(text(false, ""), more(false, ""), more(true, "")),
                  .frames = 3},
        // max_message
        test_case{.name = "a message at the limit", .input = text(true, "0123456789"), .frames = 1, .max = 10},
        test_case{.name    = "a message past the limit",
                  .input   = text(true, "0123456789!"),
                  .failure = message_too_big,
                  .max     = 10},
        test_case{.name   = "a fragmented message at the limit",
                  .input  = join(text(false, "012345"), more(true, "6789")),
                  .frames = 2,
                  .max    = 10},
        test_case{.name    = "a fragmented message past the limit",
                  .input   = join(text(false, "012345"), more(false, "6789"), more(true, "!")),
                  .failure = message_too_big,
                  .frames  = 2,
                  .max     = 10},
        test_case{.name   = "messages at the limit, one after another",
                  .input  = join(text(false, "01234"), more(true, "56789"), text(true, "0123456789")),
                  .frames = 3,
                  .max    = 10},
        test_case{.name   = "control frames don't count",
                  .input  = join(text(false, "01234"), to(r, ws::opcode::ping, true, hello), more(true, "56789")),
                  .frames = 3,
                  .max    = 10},
        // UTF-8
        test_case{.name   = "a character split across fragments",
                  .input  = join(text(false, "caf\xc3"), more(false, ""), more(true, "\xa9!")),
                  .frames = 3},
        test_case{.name   = "a 4-byte character split at each byte",
                  .input  = join(text(false, "\xf0"), more(false, "\x9f"), more(false, "\x98"), more(true, "\x80")),
                  .frames = 4},
        test_case{.name      = "an invalid byte, failing at once",
                  .input     = text(true,
                                    "ab\xff"
                                    "cdefgh"),
                  .failure   = invalid_payload,
                  .failed_at = head + 2},
        test_case{.name      = "an invalid continuation, in the next fragment",
                  .input     = join(text(false, "caf\xc3"), more(true, "(")),
                  .failure   = invalid_payload,
                  .frames    = 1,
                  .failed_at = (2 * head) + 4},
        test_case{.name      = "a surrogate",
                  .input     = text(true, "\xed\xa0\x80"),
                  .failure   = invalid_payload,
                  .failed_at = head + 1},
        test_case{.name    = "a character cut short by the message's end",
                  .input   = join(text(false, "euro "), more(true, "\xe2\x82")),
                  .failure = invalid_payload,
                  .frames  = 1},
        test_case{.name   = "binary isn't UTF-8",
                  .input  = to(r, ws::opcode::binary, true, unhex("ff c0 80 e2")),
                  .frames = 1},
        // closes
        test_case{.name    = "a close of 1 byte",
                  .input   = to(r, ws::opcode::close, true, unhex("03")),
                  .failure = protocol_error},
        test_case{.name    = "a close with 1005",
                  .input   = to(r, ws::opcode::close, true, unhex("03 ed")),
                  .failure = protocol_error},
        test_case{.name    = "a close with 1006",
                  .input   = to(r, ws::opcode::close, true, unhex("03 ee")),
                  .failure = protocol_error},
        test_case{.name    = "a close with a reason that isn't UTF-8",
                  .input   = to(r, ws::opcode::close, true, unhex("03 e8 ff")),
                  .failure = protocol_error},
        test_case{.name    = "data after the peer's close",
                  .input   = join(to(r, ws::opcode::close, true, unhex("03 e8")), text(true, "late")),
                  .failure = protocol_error,
                  .frames  = 1},
        test_case{.name    = "a ping after the peer's close",
                  .input   = join(to(r, ws::opcode::close, true, {}), to(r, ws::opcode::ping, true, {})),
                  .failure = protocol_error,
                  .frames  = 1},
        test_case{.name    = "a second close",
                  .input   = join(to(r, ws::opcode::close, true, {}), to(r, ws::opcode::close, true, {})),
                  .failure = protocol_error,
                  .frames  = 1});

    CAPTURE(r == ws::role::server ? "server" : "client", tc.name);

    const auto out = read(r, tc.input, {.max_message = tc.max});
    CHECK(out.failure == tc.failure);
    CHECK(out.events.size() == tc.frames);
    if (tc.failure && tc.failed_at != 0)
    {
        ws::session s{r, {.max_message = tc.max}};
        const auto  bytewise = test::websocket::read(s, tc.input, 1);
        CHECK(bytewise.failed_byte() == tc.failed_at);
    }
    if (!tc.failure) CHECK(out.used == tc.input.size());
}

TEST_CASE("sessions take pings and pongs between a message's fragments", "[websocket][session]")
{
    const auto r   = GENERATE(ws::role::server, ws::role::client);
    const auto out = read(r,
                          join(to(r, ws::opcode::text, false, bytes("Hel")),
                               to(r, ws::opcode::ping, true, bytes("1")),
                               to(r, ws::opcode::continuation, false, bytes("l")),
                               to(r, ws::opcode::pong, true, {}),
                               to(r, ws::opcode::ping, true, bytes("2")),
                               to(r, ws::opcode::continuation, true, bytes("o"))));

    CHECK_FALSE(out.failure);
    CHECK(out.events
          == std::vector<test::websocket::event>{
              {.what = kind::data, .type = ws::opcode::text},
              {.what = kind::ping, .type = ws::opcode::ping, .data = bytes("1")},
              {.what = kind::data, .type = ws::opcode::text},
              {.what = kind::pong, .type = ws::opcode::pong},
              {.what = kind::ping, .type = ws::opcode::ping, .data = bytes("2")},
              message(ws::opcode::text, bytes("Hello")),
    });
}

TEST_CASE("sessions keep a failure", "[websocket][session]")
{
    ws::session s{ws::role::server};
    const auto  bad = s.begin_frame({.op = ws::opcode{3}, .masked = true});
    REQUIRE_FALSE(bad.has_value());
    CHECK(bad.error() == sent(close_code::protocol_error));
    CHECK(s.failure() == bad.error());

    // receiving: everything after fails the same.
    const auto again = s.begin_frame({.op = ws::opcode::text, .masked = true});
    REQUIRE_FALSE(again.has_value());
    CHECK(again.error() == bad.error());
    CHECK(s.payload({}) == bad.error());
    CHECK(error_of(s.end_frame()) == bad.error());

    // sending: only the close that fails the connection.
    const auto data = s.frame_for(ws::opcode::text, true, 1);
    REQUIRE_FALSE(data.has_value());
    CHECK(data.error() == bad.error());
    CHECK(s.frame_for(ws::opcode::close, true, 2).has_value());
    CHECK(error_of(s.frame_for(ws::opcode::close, true, 2)) == std::errc::broken_pipe);
}

TEST_CASE("sessions check how their owner reads frames", "[websocket][session]")
{
    ws::session s{ws::role::client};
    REQUIRE(s.begin_frame({.op = ws::opcode::binary, .length = 3}).has_value());
    CHECK(s.remaining() == 3);

    // another frame before this one's over, its end before its payload's, and more payload than it has.
    CHECK(error_of(s.begin_frame({.op = ws::opcode::binary})) == std::errc::invalid_argument);
    CHECK(error_of(s.end_frame()) == std::errc::invalid_argument);
    std::array<std::byte, 4> four{};
    CHECK(s.payload(four) == std::errc::invalid_argument);

    // none of which is kept.
    CHECK_FALSE(s.failure());
    CHECK_FALSE(s.payload(std::span{four}.first(3)));
    CHECK(s.remaining() == 0);
    const auto end = s.end_frame();
    REQUIRE(end.has_value());
    CHECK(end->what == kind::message_end);
}

TEST_CASE("sessions' close handshake", "[websocket][session][close]")
{
    const auto r     = GENERATE(ws::role::server, ws::role::client);
    const auto close = [&](std::string_view hex) { return to(r, ws::opcode::close, true, unhex(hex)); };

    SECTION("the peer's close first: the reply echoes its code")
    {
        ws::session s{r};
        const auto  out = test::websocket::read(s, close("03 e9 62 79 65"), 1);
        REQUIRE_FALSE(out.failure);
        REQUIRE(out.events.size() == 1);
        CHECK(out.events[0].what == kind::close);
        CHECK(out.events[0].code == close_code::going_away);
        CHECK(out.events[0].reason == "bye");
        CHECK(s.state() == ws::close_state::close_received);

        std::array<std::byte, 2> reply{};
        CHECK(s.close_reply(reply) == 2);
        CHECK(reply == std::array{std::byte{0x03}, std::byte{0xe9}});

        // data may still go (a message being sent is finished), then the reply.
        REQUIRE(s.frame_for(ws::opcode::binary, true, 1).has_value());
        REQUIRE(s.frame_for(ws::opcode::close, true, 2).has_value());
        CHECK(s.state() == ws::close_state::closed);
        CHECK(error_of(s.frame_for(ws::opcode::ping, true, 0)) == std::errc::broken_pipe);
    }

    SECTION("the peer's empty close is 1005, and the reply has no code")
    {
        ws::session s{r};
        const auto  out = test::websocket::read(s, close(""), 1);
        REQUIRE(out.events.size() == 1);
        CHECK(out.events[0].code == close_code::no_status_received);
        CHECK(out.events[0].reason.empty());

        std::array<std::byte, 2> reply{};
        CHECK(s.close_reply(reply) == 0);
    }

    SECTION("an application's code is echoed")
    {
        ws::session s{r};
        const auto  out = test::websocket::read(s, close("0f a0"), 1);
        REQUIRE(out.events.size() == 1);
        CHECK(out.events[0].code == close_code{4000});

        std::array<std::byte, 2> reply{};
        CHECK(s.close_reply(reply) == 2);
        CHECK(reply == std::array{std::byte{0x0f}, std::byte{0xa0}});
    }

    SECTION("this side's close first: data, pings and pongs are dropped until the peer's")
    {
        ws::session s{r};
        REQUIRE(s.frame_for(ws::opcode::text, false, 1).has_value());
        REQUIRE(s.frame_for(ws::opcode::close, true, 2).has_value());
        CHECK(s.state() == ws::close_state::close_sent);
        CHECK(error_of(s.frame_for(ws::opcode::continuation, true, 1)) == std::errc::broken_pipe);
        CHECK(error_of(s.frame_for(ws::opcode::pong, true, 0)) == std::errc::broken_pipe);

        const auto mine = test::websocket::read(s,
                                                join(to(r, ws::opcode::text, false, bytes("a")),
                                                     to(r, ws::opcode::ping, true, bytes("p")),
                                                     to(r, ws::opcode::continuation, true, bytes("\xff")),
                                                     to(r, ws::opcode::binary, true, std::vector<std::byte>(300)),
                                                     to(r, ws::opcode::pong, true, {}),
                                                     close("03 e8")),
                                                7);
        REQUIRE_FALSE(mine.failure);
        REQUIRE(mine.events.size() == 6);
        for (std::size_t i = 0; i < 5; ++i)
        {
            CAPTURE(i);
            CHECK(mine.events[i].what == kind::dropped);
        }
        CHECK(mine.events[5].what == kind::close);
        CHECK(mine.events[5].code == close_code::normal_closure);
        CHECK(s.state() == ws::close_state::closed);
    }

    SECTION("after this side's close, frames are still checked")
    {
        ws::session s{r};
        REQUIRE(s.frame_for(ws::opcode::close, true, 0).has_value());

        const auto out = test::websocket::read(s, to(r, ws::opcode::continuation, true, bytes("x")), 1);
        CHECK(out.failure == sent(close_code::protocol_error));
    }

    SECTION("after this side's close, messages aren't limited")
    {
        ws::session s{r, {.max_message = 4}};
        REQUIRE(s.frame_for(ws::opcode::close, true, 0).has_value());

        const auto out = test::websocket::read(s, to(r, ws::opcode::binary, true, std::vector<std::byte>(5)), 1);
        CHECK_FALSE(out.failure);
        CHECK(out.events.size() == 1);
    }
}

TEST_CASE("sessions check what their owner sends", "[websocket][session][send]")
{
    const auto invalid = std::make_error_code(std::errc::invalid_argument);

    SECTION("control frames: at most 125 bytes, never fragmented")
    {
        ws::session s{ws::role::server};
        CHECK(s.frame_for(ws::opcode::ping, true, 125).has_value());
        CHECK(error_of(s.frame_for(ws::opcode::ping, true, 126)) == invalid);
        CHECK(error_of(s.frame_for(ws::opcode::pong, false, 0)) == invalid);
        CHECK(error_of(s.frame_for(ws::opcode::close, false, 0)) == invalid);
        CHECK(s.state() == ws::close_state::open);
    }

    SECTION("one message at a time")
    {
        ws::session s{ws::role::server};
        CHECK(error_of(s.frame_for(ws::opcode::continuation, true, 1)) == invalid);
        REQUIRE(s.frame_for(ws::opcode::text, false, 1).has_value());
        CHECK(error_of(s.frame_for(ws::opcode::text, true, 1)) == invalid);
        CHECK(error_of(s.frame_for(ws::opcode::binary, false, 1)) == invalid);
        // control frames go between its frames.
        CHECK(s.frame_for(ws::opcode::ping, true, 0).has_value());
        CHECK(s.frame_for(ws::opcode::continuation, false, 1).has_value());
        CHECK(s.frame_for(ws::opcode::continuation, true, 0).has_value());
        CHECK(error_of(s.frame_for(ws::opcode::continuation, true, 1)) == invalid);
        CHECK(s.frame_for(ws::opcode::binary, true, 1).has_value());
    }

    SECTION("reserved opcodes, and lengths of 2^63 or more")
    {
        ws::session s{ws::role::server};
        CHECK(error_of(s.frame_for(ws::opcode{3}, true, 0)) == invalid);
        CHECK(error_of(s.frame_for(ws::opcode{0xb}, true, 0)) == invalid);
        CHECK(error_of(s.frame_for(ws::opcode::binary, true, std::uint64_t{1} << 63)) == invalid);
        CHECK(s.frame_for(ws::opcode::binary, true, (std::uint64_t{1} << 63) - 1).has_value());
    }

    SECTION("nothing after a close")
    {
        ws::session s{ws::role::client};
        REQUIRE(s.frame_for(ws::opcode::close, true, 2).has_value());
        CHECK(error_of(s.frame_for(ws::opcode::text, true, 0)) == std::errc::broken_pipe);
        CHECK(error_of(s.frame_for(ws::opcode::close, true, 0)) == std::errc::broken_pipe);
    }

    SECTION("a server's frames aren't masked")
    {
        ws::session s{ws::role::server};
        const auto  header = s.frame_for(ws::opcode::binary, true, 300);
        REQUIRE(header.has_value());
        CHECK(*header == ws::frame_header{.fin = true, .op = ws::opcode::binary, .length = 300});
    }

    SECTION("a client's frames are masked, each with its own key")
    {
        ws::session                           s{ws::role::client};
        std::vector<std::array<std::byte, 4>> keys;
        for (int i = 0; i < 8; ++i)
        {
            const auto header = s.frame_for(ws::opcode::ping, true, 4);
            REQUIRE(header.has_value());
            CHECK(header->masked);
            CHECK(header->fin);
            CHECK(header->op == ws::opcode::ping);
            CHECK(header->length == 4);
            keys.push_back(header->mask_key);
        }

        // 8 random keys all the same would be a 2^-224 chance.
        CHECK(std::ranges::count(keys, keys.front()) < 8);

        // and what the session reads back, masked so, as a server's would.
        const auto header = s.frame_for(ws::opcode::text, true, 5);
        REQUIRE(header.has_value());
        ws::session server{ws::role::server};
        const auto  out = test::websocket::read(server, test::websocket::frame(*header, bytes("Hello")), 1);
        CHECK(out.events == std::vector{message(ws::opcode::text, bytes("Hello"))});
    }
}
