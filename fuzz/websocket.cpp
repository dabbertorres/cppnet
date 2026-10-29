// fuzz_websocket checks that WebSockets' frame codec and session don't crash on any frames, and that:
// - a server's session and a client's, reading the input as frames at once, a byte at a time, and in pieces of 3 and
//   7 bytes, agree: the same frames taken (the same messages, pings, pongs and closes), and the same failure, at the
//   same byte (read a byte at a time, the byte that fails is in what was being read at once when it failed).
// - the frames a session takes, encoded again (their headers as decoded, their payloads masked with their keys), are
//   the input's bytes, as headers' lengths must be in their minimal form.
// - the frames a server's session takes, sent by a client's session (whose frame_for() takes them in order, giving
//   headers with keys of its own), are taken the same by another server's; and sent unmasked, the same by a client's.
//   The frames a client's takes, sent by a server's session, have the headers they came with.
// - a close taken encodes (encode_close()) to its payload again.
// - any header decode_header() takes encodes to the bytes it took; and masking the input in pieces is masking it at
//   once.
//
// The input's first byte is the sessions' configuration: its low 7 bits, if not 0, are limits::max_message in 4s;
// its high bit, set, has the reading sessions send a close before they read (so data, pings and pongs are dropped).
// The rest is frames. (util::utf8_validator has fuzz_utf8.)
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <print>
#include <span>
#include <vector>

#include "websocket/frame.hpp"
#include "websocket/frames.hpp"
#include "websocket/session.hpp"

namespace
{

namespace ws = net::websocket::detail;

[[noreturn]] void fail(const char* what, std::span<const std::byte> input)
{
    std::print(stderr, "PROPERTY FAILED: {}\n  input:", what);
    for (const auto b : input) std::print(stderr, " {:02x}", std::to_integer<unsigned>(b));
    std::println(stderr);
    std::abort();
}

struct config
{
    ws::limits limits{};
    bool       closed_first = false;
};

ws::session make_session(ws::role r, const config& c)
{
    ws::session s{r, c.limits};
    if (c.closed_first && !s.frame_for(ws::opcode::close, true, 0)) std::abort();
    return s;
}

test::websocket::trace read(ws::role r, const config& c, std::span<const std::byte> frames, std::size_t piece)
{
    auto s = make_session(r, c);
    return test::websocket::read(s, frames, piece);
}

// encoded is the frames encoded again: their headers as they are, their payloads masked with their keys.
std::vector<std::byte> encoded(const std::vector<test::websocket::read_frame>& frames)
{
    std::vector<std::byte> out;
    for (const auto& f : frames)
    {
        const auto bytes = test::websocket::frame(f.header, f.payload);
        out.insert(out.end(), bytes.begin(), bytes.end());
    }
    return out;
}

// read_agreeing reads frames as a session of role r, in pieces of several sizes, which must agree; it returns what it
// read at once.
test::websocket::trace
read_agreeing(ws::role r, const config& c, std::span<const std::byte> frames, std::span<const std::byte> input)
{
    auto whole = read(r, c, frames, std::numeric_limits<std::size_t>::max());

    for (const std::size_t piece : {std::size_t{1}, std::size_t{3}, std::size_t{7}})
    {
        const auto other = read(r, c, frames, piece);
        if (other.events != whole.events) fail("read in pieces, the frames taken differ", input);
        if (other.failure != whole.failure) fail("read in pieces, the failure differs", input);
        if (other.used != whole.used) fail("read in pieces, the frames used differ", input);
        if (whole.failure && piece == 1)
        {
            const auto at = other.failed_byte();
            if (at < whole.failed_from || at >= whole.failed_to)
                fail("read a byte at a time, it failed at another byte", input);
        }
    }

    if (encoded(whole.frames)
        != std::vector<std::byte>(frames.begin(), frames.begin() + static_cast<std::ptrdiff_t>(whole.used)))
        fail("the frames taken, encoded again, aren't the input", input);

    for (const auto& e : whole.events)
    {
        if (e.what != ws::received::kind::close) continue;

        std::array<std::byte, ws::max_control_payload> out{};
        const auto                                     size = ws::encode_close(e.code, e.reason, out);
        if (!size || std::vector<std::byte>(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(*size)) != e.data)
            fail("a close taken doesn't encode to its payload", input);
    }
    return whole;
}

// resent is the frames, as a session of role r sends them (failing if it refuses one): with its headers' masks.
std::vector<test::websocket::read_frame>
resent(ws::role r, const std::vector<test::websocket::read_frame>& frames, std::span<const std::byte> input)
{
    ws::session                              sender{r};
    std::vector<test::websocket::read_frame> out;
    for (const auto& f : frames)
    {
        const auto header = sender.frame_for(f.header.op, f.header.fin, f.header.length);
        if (!header) fail("a frame taken can't be sent in turn", input);
        if (header->masked != (r == ws::role::client))
            fail("a frame sent is masked otherwise than its role says", input);
        out.push_back({.header = *header, .payload = f.payload});
    }
    return out;
}

void check_roles(const config& c, std::span<const std::byte> frames, std::span<const std::byte> input)
{
    const auto server = read_agreeing(ws::role::server, c, frames, input);
    const auto client = read_agreeing(ws::role::client, c, frames, input);

    // what the server took, from a client's session, and unmasked.
    const auto from_client = encoded(resent(ws::role::client, server.frames, input));
    const auto again       = read(ws::role::server, c, from_client, std::numeric_limits<std::size_t>::max());
    if (again.failure || again.events != server.events)
        fail("a client's frames, masked again, are taken otherwise", input);

    auto unmasked = server.frames;
    for (auto& f : unmasked) f.header.masked = false;
    const auto to_client = read(ws::role::client, c, encoded(unmasked), std::numeric_limits<std::size_t>::max());
    if (to_client.failure || to_client.events != server.events)
        fail("a server's frames, taken by a client's session, are taken otherwise", input);

    // what the client took, from a server's session.
    const auto from_server = resent(ws::role::server, client.frames, input);
    for (std::size_t i = 0; i < from_server.size(); ++i)
    {
        if (from_server[i].header != client.frames[i].header) fail("a server's session sends other headers", input);
    }
}

void check_codec(std::span<const std::byte> input)
{
    if (const auto decoded = ws::decode_header(input); decoded && *decoded)
    {
        std::array<std::byte, ws::max_header_size> out{};
        const auto                                 size = ws::encode_header((*decoded)->header, out);
        if (size != (*decoded)->size || !std::ranges::equal(std::span{out}.first(size), input.first(size)))
            fail("a header decoded doesn't encode to its bytes", input);
    }

    if (input.size() < 5) return;

    const std::array key{input[0], input[1], input[2], input[3]};
    const auto       piece = (std::to_integer<std::size_t>(input[4]) % 13) + 1;

    std::vector<std::byte> whole(input.begin(), input.end());
    ws::apply_mask(whole, key, 0);

    std::vector<std::byte> pieces(input.begin(), input.end());
    for (std::size_t at = 0; at < pieces.size(); at += piece)
        ws::apply_mask(std::span{pieces}.subspan(at, std::min(piece, pieces.size() - at)), key, at);
    if (pieces != whole) fail("masked in pieces, it's masked otherwise", input);
}

}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    const auto input = std::as_bytes(std::span{data, size});
    check_codec(input);
    if (input.empty()) return 0;

    const auto first = std::to_integer<unsigned>(input[0]);
    config     c{.closed_first = (first & 0x80) != 0};
    if ((first & 0x7f) != 0) c.limits.max_message = std::uint64_t{first & 0x7f} * 4;

    check_roles(c, input.subspan(1), input);
    return 0;
}
