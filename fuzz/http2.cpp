// fuzz_http2 checks that an HTTP/2 session (as a server, and as a client) doesn't crash on any frames, nor on what its
// application does between them, and that it receives frames the same whether they arrive at once or a byte at a
// time: the same events, the same output (frames to send), and the same outcome. And that the header blocks it sends
// decode, by a decoder that takes the SETTINGS_HEADER_TABLE_SIZE of each SETTINGS the session has acknowledged (from
// the acknowledgement on, as the peer's would), to the fields the application submitted, in order: so its encoder's
// dynamic table (indexing::automatic) and size updates stay in step with what the peer allows.
//
// The input is a script, of records that each start with an op byte (its low 3 bits):
// - 0: receive the next n bytes (n is the next byte; 0, or more than there are: the rest), as frames.
// - 1: time passes: the next byte's tens of milliseconds (for the token buckets).
// - 2: headers (the next byte: bits 0-3 choose a stream, 4-5 what, 6 ends the stream): a server responds (a 200, one
//      with a Content-Length of 3, a 103, or trailers), a client opens a stream for a request (GET, POST with a
//      Content-Length of 3, or HEAD) or sends trailers.
// - 3: data (the next byte: bits 0-3 choose a stream, 7 ends the stream; the one after, its size in 64s).
// - 4: reset (the next byte: bits 0-3 choose a stream, 4-7 the code).
// - 5: consume (the next byte chooses a stream; the one after, how much, in 256s).
// - 6: PING, or GOAWAY (the next byte: bit 0 which, 1 whether the last stream is the peer's last or the most, 4-7 the
//      code).
// - 7, with the op's bit 5: submit a PRIORITY_UPDATE (the next byte: bits 0-3 choose a stream, 4-7 its urgency, past 7
//      too; the op's bit 6, incremental): a client's, which is sent only if it changes what the stream last said, and
//      replaces one not yet taken (a server's fails). The output's taken after every record but these, so several may
//      wait in it, to be replaced, or taken back (e.g. as the next record receives the end of a stream).
// - 7, without, with the op's bit 6: ORIGIN (RFC 8336), as the op's bit 7 says:
//      - 0: receive one (the next byte: bit 0 on stream 1 rather than 0, bit 1 with a flag RFC 8336 reserves (bits 2-3:
//        which), bit 4 with one it doesn't (0x80), bits 5-6 how many of "https://a.example", "https://b.example:8443"
//        and "" as its entries, bit 7 then the next n bytes, n being the byte after, as more of its payload: random
//        entries, whose lengths may run past it);
//      - 1: submit one (the next byte: bits 0-1 how many of "https://a.example", "https://b.example:8443" and
//        "https://[::1]", bits 2-3 how many origins of 6000 bytes after them, so that they're split into frames): a
//        server's, as many frames as they need (a client's fails).
// - 7, without either: receive a frame that random bytes rarely make, as the op's bits 3-4 say:
//      - 0: SETTINGS changing SETTINGS_HEADER_TABLE_SIZE to the next byte's 64s (255: 65536), for the session's
//        encoder to follow;
//      - 1: SETTINGS with NO_RFC7540_PRIORITIES of the next byte (0, 1, or a value that's invalid);
//      - 2: PRIORITY_UPDATE (the next byte: bits 0-3 choose a stream, 4-5 how: one the application knows, an idle one
//        (that many past the peer's highest), an even one, or 0; 6-7 its value: "u=1", "u=6, i", one that doesn't
//        parse, or the next n bytes, n being the byte after);
//      - 3: a malformed PRIORITY_UPDATE: on stream 1 (the next byte's bit 0), or shorter than its stream id (bits 1-2:
//        its length).
// A stream is chosen from those the application knows (a server's requests, a client's own), in order.
//
// Each output is checked to be whole frames, its PRIORITY_UPDATEs on stream 0 with values in to_string()'s form (so
// one replaced or taken back in place has left the frames around it whole), and its ORIGINs (only a server's) on stream
// 0, without flags, their entries filling them exactly.
//
// The server's script follows the client's preface and SETTINGS, and the client's the server's SETTINGS, each with
// only NO_RFC7540_PRIORITIES (1, as both sides send it), so that the fuzzer starts past them. The client opens streams
// before it receives: a GET (1), a POST with a Content-Length of 3 and no body yet (3), a HEAD (5), and a GET with a
// Priority field of two lines, "u=1" and "i" (7).
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
#include <limits>
#include <print>
#include <ratio>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "http/hpack.hpp"
#include "http/http2_config.hpp"
#include "http/http2_session.hpp"
#include "http/priority.hpp"

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
    std::vector<std::uint32_t>    table_sizes; // the peer's SETTINGS_HEADER_TABLE_SIZE after each of its SETTINGS

    void
    on_headers(http2::stream_id stream, http2::block_kind kind, std::span<const hpack::field> fields, bool end) override
    {
        log += std::format("headers {} {} {}:", stream, static_cast<int>(kind), end);
        for (const auto& f : fields) log += std::format(" {}={}", f.name, f.value);
        log += '\n';

        if (std::ranges::find(known, stream) == known.end()) known.push_back(stream);
    }

    void on_header_list_too_large(http2::stream_id stream, bool end) override
    {
        log += std::format("too large {} {}\n", stream, end);
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
    void on_settings(const http2::settings& peer) override
    {
        log += "settings\n";
        table_sizes.push_back(peer.header_table_size);
    }

    void on_goaway(http2::stream_id last, std::error_code err, std::string_view debug) override
    {
        log += std::format("goaway {} {} {}\n", last, err.value(), debug.size());
    }

    void on_ping_ack(std::span<const std::byte, 8> /*opaque*/) override { log += "ping ack\n"; }

    void on_priority_update(http2::stream_id stream, net::http::priority p) override
    {
        log += std::format("priority {} {} {}\n", stream, p.urgency, p.incremental);
    }

    void on_origin(std::span<const std::string_view> origins) override
    {
        log += "origin";
        for (const auto o : origins) log += std::format(" [{}]", o);
        log += '\n';
    }
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

// blocks_decode is whether the header blocks in a session's output decode, as the peer would decode them, to the fields
// submitted, in order: the peer's decoder's limit is the SETTINGS_HEADER_TABLE_SIZE of each SETTINGS the session
// acknowledged (table_sizes, in order), from its acknowledgement on.
bool blocks_decode(http2::role                                    role,
                   std::span<const std::byte>                     output,
                   std::span<const std::uint32_t>                 table_sizes,
                   std::span<const std::span<const hpack::field>> submitted)
{
    if (role == http2::role::client) output = output.subspan(http2::preface.size());

    hpack::decoder         dec{hpack::default_table_size, std::numeric_limits<std::size_t>::max()};
    std::size_t            acks    = 0;
    std::size_t            decoded = 0;
    std::vector<std::byte> block;

    while (output.size() >= http2::frame_header_size)
    {
        const auto h       = http2::decode_frame_header(output.first<http2::frame_header_size>());
        const auto payload = output.subspan(http2::frame_header_size, h.length);
        output             = output.subspan(http2::frame_header_size + h.length);

        const auto type = static_cast<http2::frame_type>(h.type);
        if (type == http2::frame_type::settings && (h.flags & http2::flags::ack) != 0)
        {
            if (acks < table_sizes.size()) dec.set_max_table_size(table_sizes[acks]);
            ++acks;
            continue;
        }
        if (type != http2::frame_type::headers && type != http2::frame_type::continuation) continue;

        // NOTE: the session never pads, nor sends a priority.
        if (type == http2::frame_type::headers) block.clear();
        block.insert(block.end(), payload.begin(), payload.end());
        if ((h.flags & http2::flags::end_headers) == 0) continue;

        if (decoded == submitted.size()) return false;
        const auto            expected = submitted[decoded++];
        std::size_t           at       = 0;
        bool                  same     = true;
        const std::error_code err      = dec.decode(block,
                                                    [&](const hpack::field& f)
                                                    {
                                                   same = same && at < expected.size() && f.name == expected[at].name
                                                       && f.value == expected[at].value;
                                                   ++at;
                                                    });
        if (err || !same || at != expected.size()) return false;
    }

    return decoded == submitted.size();
}

// whole_entries is whether an ORIGIN frame's payload is whole entries (a 16-bit length and that many bytes), nothing
// left over.
bool whole_entries(std::span<const std::byte> payload)
{
    while (payload.size() >= 2)
    {
        const auto length = (std::to_integer<std::size_t>(payload[0]) << 8U) | std::to_integer<std::size_t>(payload[1]);
        if (length > payload.size() - 2) return false;
        payload = payload.subspan(2 + length);
    }
    return payload.empty();
}

// whole_frames is whether a session's output is whole frames, nothing left over, its PRIORITY_UPDATEs on stream 0 with
// values in to_string()'s form (so one replaced or taken back in place has left the frames around it whole), and its
// ORIGINs a server's, on stream 0, without flags, their entries filling them exactly.
bool whole_frames(http2::role role, std::span<const std::byte> output)
{
    if (role == http2::role::client) output = output.subspan(http2::preface.size());

    while (output.size() >= http2::frame_header_size)
    {
        const auto h = http2::decode_frame_header(output.first<http2::frame_header_size>());
        if (output.size() < http2::frame_header_size + h.length) return false;
        const auto payload = output.subspan(http2::frame_header_size, h.length);
        output             = output.subspan(http2::frame_header_size + h.length);
        if (h.type == static_cast<std::uint8_t>(http2::frame_type::origin))
        {
            if (role != http2::role::server || h.stream != 0 || h.flags != 0 || !whole_entries(payload)) return false;
            continue;
        }
        if (h.type != static_cast<std::uint8_t>(http2::frame_type::priority_update)) continue;

        if (h.stream != 0 || payload.size() < 4) return false;
        const auto value = std::string_view{reinterpret_cast<const char*>(payload.data()) + 4, payload.size() - 4};
        if (net::http::to_string(net::http::parse_priority(value)) != value) return false;
    }
    return output.empty();
}

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
            open(get_prioritized, true);
        }
    }

    outcome run(std::span<const std::byte> prefix, std::span<const std::byte> input)
    {
        receive(prefix);

        script sc{input};
        while (!sc.done())
        {
            // (the output's taken after each step but a PRIORITY_UPDATE's, so several may wait in it to be replaced
            // or taken back.)
            if (!step(sc)) collect();
        }

        out.unused = pending.size();
        out.events = std::move(events.log);
        collect();

        if (!whole_frames(role, out.output) || !blocks_decode(role, out.output, events.table_sizes, submitted))
        {
            std::println(
                stderr,
                "PROPERTY FAILED: the {}'s output isn't whole frames, or its header blocks don't decode to what it "
                "submitted\n{}",
                role == http2::role::server ? "server" : "client",
                out.events);
            std::abort();
        }
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
    static constexpr std::array get_prioritized{
        hpack::field{   .name = ":method",         .value = "GET"},
        hpack::field{   .name = ":scheme",       .value = "https"},
        hpack::field{.name = ":authority", .value = "example.com"},
        hpack::field{     .name = ":path",           .value = "/"},
        hpack::field{  .name = "priority",         .value = "u=1"},
        hpack::field{  .name = "priority",           .value = "i"},
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
        if (!err) submitted.push_back(fields);
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
        const auto                  id  = stream(arg & 0xfU);
        const auto                  err = s.submit_headers(id, blocks.at(what), end);
        if (!err) submitted.push_back(blocks.at(what));
        events.log += std::format("submit headers {} {}\n", id, err.value());
    }

    void submit_data(std::uint8_t arg, std::uint8_t size)
    {
        const auto id = stream(arg & 0xfU);
        data.assign(std::size_t{64} * size, std::byte{'x'});

        const auto sent = s.submit_data(id, data, (arg & 0x80U) != 0);
        events.log += sent.has_value() ? std::format("submit data {} {}\n", id, *sent)
                                       : std::format("submit data {} error {}\n", id, sent.error().value());
    }

    void submit_priority_update(bool incremental, std::uint8_t arg)
    {
        const auto id = stream(arg & 0xfU);
        const auto sent =
            s.submit_priority_update(id, {.urgency = static_cast<std::uint8_t>(arg >> 4U), .incremental = incremental});
        events.log += sent.has_value() ? std::format("submit priority {} {}\n", id, *sent)
                                       : std::format("submit priority {} error {}\n", id, sent.error().value());
    }

    // origin receives an ORIGIN frame, or submits one, as op 7's bit 7 (which's bit 4) says.
    void origin(std::uint8_t which, script& sc)
    {
        static const std::array<std::string, 3> named{"https://a.example", "https://b.example:8443", ""};
        const auto                              arg = sc.next();

        if ((which & 0x10U) == 0)
        {
            std::uint8_t flags = 0;
            if ((arg & 2U) != 0) flags |= static_cast<std::uint8_t>(1U << ((arg >> 2U) & 3U));
            if ((arg & 0x10U) != 0) flags |= 0x80U;

            std::vector<std::byte> payload;
            for (std::size_t i = 0; i < ((arg >> 5U) & 3U); ++i)
            {
                const auto& o = named.at(i);
                payload.push_back(static_cast<std::byte>(o.size() >> 8U));
                payload.push_back(static_cast<std::byte>(o.size()));
                const auto bytes = std::as_bytes(std::span{o});
                payload.insert(payload.end(), bytes.begin(), bytes.end());
            }
            if ((arg & 0x80U) != 0)
            {
                const auto more = sc.take(sc.next());
                payload.insert(payload.end(), more.begin(), more.end());
            }

            std::vector<std::byte> frame;
            http2::encode_frame_header({.length = static_cast<std::uint32_t>(payload.size()),
                                        .type   = static_cast<std::uint8_t>(http2::frame_type::origin),
                                        .flags  = flags,
                                        .stream = (arg & 1U) != 0 ? 1U : 0U},
                                       frame);
            frame.insert(frame.end(), payload.begin(), payload.end());
            receive(frame);
            return;
        }

        static const std::array<std::string, 3> submitted{"https://a.example",
                                                          "https://b.example:8443",
                                                          "https://[::1]"};
        static const std::string                long_origin = "https://" + std::string(5'992, 'x');

        std::vector<std::string> origins(submitted.begin(), submitted.begin() + (arg & 3U));
        origins.insert(origins.end(), (arg >> 2U) & 3U, long_origin);
        const auto err = s.submit_origin(origins);
        events.log += std::format("submit origin {} {}\n", origins.size(), err.value());
    }

    // rare_frame receives one of the frames random bytes rarely make (op 7's).
    void rare_frame(std::uint8_t which, script& sc)
    {
        if ((which & 8U) != 0)
        {
            origin(which, sc);
            return;
        }

        std::vector<std::byte> frame;
        switch (which & 3U)
        {
        case 0:
        {
            const auto size = sc.next();
            http2::encode_settings({.header_table_size = size == 255 ? 65'536U : 64U * size}, {}, frame);
            break;
        }

        case 1:
        {
            const std::array<std::byte, 6> setting{std::byte{0},
                                                   std::byte{9},
                                                   std::byte{0},
                                                   std::byte{0},
                                                   std::byte{0},
                                                   std::byte{sc.next()}};
            http2::encode_frame_header(
                {.length = 6, .type = static_cast<std::uint8_t>(http2::frame_type::settings), .stream = 0},
                frame);
            frame.insert(frame.end(), setting.begin(), setting.end());
            break;
        }

        case 2:
        {
            const auto       arg      = sc.next();
            const auto       selector = arg & 0xfU;
            http2::stream_id id       = 0;
            switch ((arg >> 4U) & 3U)
            {
            case 0: id = stream(selector); break;
            case 1: id = (s.last_peer_stream() == 0 ? 1U : s.last_peer_stream() + 2U) + (2U * selector); break;
            case 2: id = 2U * (selector + 1U); break;
            default: break;
            }

            constexpr std::array<std::string_view, 3> values{"u=1", "u=6, i", "u="};
            std::string_view                          value;
            std::span<const std::byte>                raw;
            if (const auto v = (arg >> 6U) & 3U; v < values.size()) value = values.at(v);
            else raw = sc.take(sc.next());

            const auto length = static_cast<std::uint32_t>(4 + value.size() + raw.size());
            http2::encode_frame_header(
                {.length = length, .type = static_cast<std::uint8_t>(http2::frame_type::priority_update), .stream = 0},
                frame);
            for (int shift = 24; shift >= 0; shift -= 8) frame.push_back(static_cast<std::byte>(id >> shift));
            const auto bytes = std::as_bytes(std::span{value});
            frame.insert(frame.end(), bytes.begin(), bytes.end());
            frame.insert(frame.end(), raw.begin(), raw.end());
            break;
        }

        default:
        {
            const auto arg       = sc.next();
            const bool on_stream = (arg & 1U) != 0;
            const auto length    = on_stream ? 7U : (arg >> 1U) & 3U;
            const auto stream_id = on_stream ? 1U : 0U;
            http2::encode_frame_header({.length = length,
                                        .type   = static_cast<std::uint8_t>(http2::frame_type::priority_update),
                                        .stream = stream_id},
                                       frame);
            frame.insert(frame.end(),
                         {std::byte{0},
                          std::byte{0},
                          std::byte{0},
                          std::byte{1},
                          std::byte{'u'},
                          std::byte{'='},
                          std::byte{'1'}});
            frame.resize(http2::frame_header_size + length);
            break;
        }
        }
        receive(frame);
    }

    // step runs the script's next record, returning whether it was a submitted PRIORITY_UPDATE.
    bool step(script& sc)
    {
        const auto op = sc.next();
        switch (op & 7U)
        {
        case 0: receive(sc.take(sc.next())); break;

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

        case 7:
            if ((op & 0x20U) != 0)
            {
                submit_priority_update((op & 0x40U) != 0, sc.next());
                return true;
            }
            rare_frame(op >> 3U, sc);
            break;

        default: break;
        }
        return false;
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

    std::vector<std::span<const hpack::field>> submitted; // the blocks submitted, in order (static arrays' fields)
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
    http2::encode_settings({.no_rfc7540_priorities = true}, {}, prefix);
    check(http2::role::server, prefix, input);

    // a client, receiving a server's SETTINGS first.
    check(http2::role::client, std::span{prefix}.subspan(preface.size()), input);
    return 0;
}
