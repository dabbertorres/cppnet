#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "coro/task.hpp"
#include "io/io.hpp"
#include "io/reader.hpp"
#include "io/writer.hpp"
#include "websocket/error.hpp"
#include "websocket/frame.hpp"
#include "websocket/frames.hpp"

// A scripted WebSocket client, for the server's tests: it sends a handshake and frames by hand (masked or not, at once
// or a byte at a time), and reads what comes back, frame by frame, as it came (no session judges it), over any
// connection the test made (a TCP socket, or a TLS stream).
namespace test::websocket
{

// key is RFC 6455's example key (section 4.2.1), whose accept is s3pPLMBiTxaQ9kYGzzhZRbK+xOo=.
inline constexpr std::string_view key = "dGhlIHNhbXBsZSBub25jZQ==";

// handshake is a handshake request for target, with extra lines (each ending in CRLF) before the empty line.
inline std::string handshake(std::string_view target = "/ws", std::string_view extra = {})
{
    return std::format("GET {} HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                       "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: {}\r\n{}\r\n",
                       target,
                       key,
                       extra);
}

// mask_key is the key the peer masks its frames with.
inline constexpr std::array<std::byte, 4> mask_key{std::byte{0x37}, std::byte{0xfa}, std::byte{0x21}, std::byte{0x3d}};

// client_frame is a frame as a client sends it: masked (unless told not to), its payload's length set.
inline std::vector<std::byte>
client_frame(ws::opcode op, std::span<const std::byte> payload, bool fin = true, bool masked = true)
{
    return frame({.fin = fin, .op = op, .masked = masked, .mask_key = mask_key}, payload);
}

inline std::vector<std::byte> client_frame(ws::opcode op, std::string_view payload, bool fin = true, bool masked = true)
{
    return client_frame(op, bytes(payload), fin, masked);
}

// close_frame is a client's close with code and reason.
inline std::vector<std::byte> close_frame(net::websocket::close_code code, std::string_view reason = {})
{
    std::array<std::byte, ws::max_control_payload> payload{};
    const auto                                     size = ws::encode_close(code, reason, payload).value_or(0);
    return client_frame(ws::opcode::close, std::span{payload}.first(size));
}

// received_frame is a frame the server sent: its header, and payload.
struct received_frame
{
    ws::frame_header       header;
    std::vector<std::byte> payload;

    [[nodiscard]] std::string text() const { return {reinterpret_cast<const char*>(payload.data()), payload.size()}; }

    // close is a close's payload, parsed.
    [[nodiscard]] ws::close_payload close() const { return ws::parse_close(payload).value_or(ws::close_payload{}); }
};

// raw_peer is the client's side of a connection.
class raw_peer
{
public:
    raw_peer(net::io::reader& in, net::io::writer& out)
        : in{&in}
        , out{&out}
    {}

    // send sends bytes, at once, or (with piece) piece bytes per write.
    net::coro::task<std::error_code> send(std::span<const std::byte> data, std::size_t piece = 0)
    {
        if (piece == 0) piece = data.size();
        for (std::size_t at = 0; at < data.size(); at += piece)
        {
            auto wrote = co_await net::io::write_all(*out, data.subspan(at, std::min(piece, data.size() - at)));
            if (wrote.err) co_return wrote.err;
        }
        co_return std::error_code{};
    }

    net::coro::task<std::error_code> send(std::string_view text) { co_return co_await send(bytes(text)); }

    // read_head reads the response's head (up to the empty line), keeping what came after it for frames.
    net::coro::task<std::expected<std::string, std::error_code>> read_head()
    {
        while (true)
        {
            const std::string_view seen{reinterpret_cast<const char*>(buffer.data()), buffer.size()};
            if (const auto end = seen.find("\r\n\r\n"); end != std::string_view::npos)
            {
                std::string head{seen.substr(0, end + 4)};
                buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(end + 4));
                co_return head;
            }

            if (auto err = co_await fill()) co_return std::unexpected(err);
        }
    }

    // read_frame reads the next frame the server sent.
    net::coro::task<std::expected<received_frame, std::error_code>> read_frame()
    {
        while (true)
        {
            const auto decoded = ws::decode_header(buffer);
            if (!decoded) co_return std::unexpected(decoded.error());
            if (*decoded && buffer.size() - (*decoded)->size >= (*decoded)->header.length)
            {
                const auto  size   = (*decoded)->size;
                const auto  length = static_cast<std::size_t>((*decoded)->header.length);
                const auto* start  = buffer.data() + size;

                received_frame f{
                    .header  = (*decoded)->header,
                    .payload = {start, start + length}
                };
                if (f.header.masked) ws::apply_mask(f.payload, f.header.mask_key, 0);
                buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(size + length));
                co_return f;
            }

            if (auto err = co_await fill()) co_return std::unexpected(err);
        }
    }

    // read_until_end reads frames until the connection ends, returning them and how it ended (with what's left of a
    // frame cut short).
    net::coro::task<std::pair<std::vector<received_frame>, std::error_code>> read_until_end()
    {
        std::vector<received_frame> frames;
        while (true)
        {
            auto f = co_await read_frame();
            if (!f) co_return std::pair{std::move(frames), f.error()};
            frames.push_back(std::move(*f));
        }
    }

    // left is what's been read but not taken as a frame (or the head).
    [[nodiscard]] std::size_t left() const noexcept { return buffer.size(); }

private:
    net::coro::task<std::error_code> fill()
    {
        std::array<std::byte, 16'384> chunk{};
        auto                          res = co_await in->read(chunk);
        buffer.insert(buffer.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(res.count));
        if (res.count > 0) co_return std::error_code{};
        co_return res.err ? res.err : make_error_code(net::io::errc::no_progress);
    }

    net::io::reader*       in;
    net::io::writer*       out;
    std::vector<std::byte> buffer;
};

}
