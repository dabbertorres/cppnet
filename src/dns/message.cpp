#include "dns/message.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include "dns/error.hpp"
#include "ipv4_addr.hpp"
#include "ipv6_addr.hpp"

namespace
{

namespace dns = net::dns;

// The header's flags: the third and fourth octets.
constexpr std::uint8_t qr_bit = 0x80;
constexpr std::uint8_t aa_bit = 0x04;
constexpr std::uint8_t tc_bit = 0x02;
constexpr std::uint8_t rd_bit = 0x01;
constexpr std::uint8_t ra_bit = 0x80;
constexpr std::uint8_t ad_bit = 0x20;

// A label's length octet's top two bits: a label (00) or a compression pointer (11). 01 and 10 are reserved.
constexpr std::uint8_t label_kind_mask = 0xc0;
constexpr std::uint8_t pointer_kind    = 0xc0;

// The DO bit, in an OPT record's TTL.
constexpr std::uint32_t do_bit = 0x8000;

// The smallest a question (the root's name, a type and a class) and a record (with a TTL and an RDLENGTH) can be: to
// bound how much counts reserve.
constexpr std::size_t min_question_size = 5;
constexpr std::size_t min_record_size   = 11;

constexpr std::size_t max_count = std::numeric_limits<std::uint16_t>::max();

void put8(std::uint8_t value, std::vector<std::byte>& out) { out.push_back(static_cast<std::byte>(value)); }

void put16(std::uint16_t value, std::vector<std::byte>& out)
{
    put8(static_cast<std::uint8_t>(value >> 8U), out);
    put8(static_cast<std::uint8_t>(value), out);
}

void put32(std::uint32_t value, std::vector<std::byte>& out)
{
    put16(static_cast<std::uint16_t>(value >> 16U), out);
    put16(static_cast<std::uint16_t>(value), out);
}

// encode_name appends name's labels to out, returning false (with out partly written) if it isn't a valid name.
bool encode_name(std::string_view name, std::vector<std::byte>& out)
{
    if (name.ends_with('.')) name.remove_suffix(1);

    std::size_t size = 1; // the root's octet
    while (!name.empty())
    {
        const auto end   = name.find('.');
        const auto label = name.substr(0, end);
        if (label.empty() || label.size() > dns::max_label_size) return false;

        size += 1 + label.size();
        if (size > dns::max_name_size) return false;

        put8(static_cast<std::uint8_t>(label.size()), out);
        const auto bytes = std::as_bytes(std::span{label});
        out.insert(out.end(), bytes.begin(), bytes.end());

        if (end == std::string_view::npos) break;
        name.remove_prefix(end + 1);
        if (name.empty()) return false; // a name ending in two dots, whose last label is empty
    }

    put8(0, out);
    return true;
}

bool encode_record(const dns::record& r, std::vector<std::byte>& out)
{
    if (!encode_name(r.name, out)) return false;

    put16(static_cast<std::uint16_t>(r.type()), out);
    put16(static_cast<std::uint16_t>(r.rclass), out);
    put32(r.ttl, out);

    if (const auto* addr = std::get_if<net::ipv4_addr>(&r.data))
    {
        put16(4, out);
        put32(static_cast<std::uint32_t>(*addr), out);
    }
    else if (const auto* addr6 = std::get_if<net::ipv6_addr>(&r.data))
    {
        put16(16, out);
        for (const auto b : std::span<const std::uint8_t, 16>{static_cast<const std::uint8_t*>(*addr6), 16})
            put8(b, out);
    }
    else if (const auto* alias = std::get_if<dns::cname>(&r.data))
    {
        const auto length_at = out.size();
        put16(0, out);
        if (!encode_name(alias->target, out)) return false;

        // a name is at most 255 octets.
        const auto length  = out.size() - length_at - 2;
        out[length_at]     = static_cast<std::byte>(length >> 8U);
        out[length_at + 1] = static_cast<std::byte>(length);
    }

    return true;
}

// encode_header appends msg's header, with its RCODE's low 4 bits, and its sections' counts (additionals counting the
// OPT record).
void encode_header(const dns::message& msg, std::size_t additionals, std::vector<std::byte>& out)
{
    const auto& h      = msg.header;
    const auto  opcode = static_cast<unsigned>(h.opcode);
    const auto  rcode  = static_cast<unsigned>(h.rcode);

    put16(h.id, out);
    put8(static_cast<std::uint8_t>((h.response ? qr_bit : 0U) | (opcode << 3U) | (h.authoritative ? aa_bit : 0U)
                                   | (h.truncated ? tc_bit : 0U) | (h.recursion_desired ? rd_bit : 0U)),
         out);
    put8(static_cast<std::uint8_t>((h.recursion_available ? ra_bit : 0U) | (h.authentic_data ? ad_bit : 0U)
                                   | (rcode & 0x0fU)),
         out);
    put16(static_cast<std::uint16_t>(msg.questions.size()), out);
    put16(static_cast<std::uint16_t>(msg.answers.size()), out);
    put16(static_cast<std::uint16_t>(msg.authorities.size()), out);
    put16(static_cast<std::uint16_t>(additionals), out);
}

// encode_sections appends msg's questions and records, but the OPT record, returning false if a name isn't valid.
bool encode_sections(const dns::message& msg, std::vector<std::byte>& out)
{
    for (const auto& q : msg.questions)
    {
        if (!encode_name(q.name, out)) return false;
        put16(static_cast<std::uint16_t>(q.type), out);
        put16(static_cast<std::uint16_t>(q.rclass), out);
    }

    for (const auto* section : {&msg.answers, &msg.authorities, &msg.additionals})
    {
        for (const auto& r : *section)
            if (!encode_record(r, out)) return false;
    }

    return true;
}

// encode_opt appends an OPT record of e's, with rcode's upper 8 bits.
void encode_opt(const dns::edns& e, dns::rcode rcode, std::vector<std::byte>& out)
{
    put8(0, out); // the root
    put16(static_cast<std::uint16_t>(dns::type::opt), out);
    put16(e.udp_size, out);
    put32(((static_cast<std::uint32_t>(rcode) >> 4U) << 24U) | (std::uint32_t{e.version} << 16U)
              | (e.dnssec_ok ? do_bit : 0U),
          out);
    put16(0, out); // no options
}

// reader reads a message from the front, every read bounds-checked.
struct reader
{
    std::span<const std::byte> msg;
    std::size_t                pos = 0;

    [[nodiscard]] std::size_t left() const noexcept { return msg.size() - pos; }

    [[nodiscard]] std::uint8_t at(std::size_t offset) const noexcept
    {
        return std::to_integer<std::uint8_t>(msg[offset]);
    }

    bool u8(std::uint8_t& value) noexcept
    {
        if (left() < 1) return false;
        value = at(pos++);
        return true;
    }

    bool u16(std::uint16_t& value) noexcept
    {
        if (left() < 2) return false;
        value = static_cast<std::uint16_t>((at(pos) << 8U) | at(pos + 1));
        pos += 2;
        return true;
    }

    bool u32(std::uint32_t& value) noexcept
    {
        std::uint16_t high = 0;
        std::uint16_t low  = 0;
        if (left() < 4 || !u16(high) || !u16(low)) return false;
        value = (std::uint32_t{high} << 16U) | low;
        return true;
    }

    // name decodes a name at pos into out, following compression pointers, and moves pos past it (the first pointer,
    // if it has one). Each pointer must point before where the name has been read from so far: before the name, and
    // then before the last pointer's target, so they can't loop; and there may be at most max_pointers.
    bool name(std::string& out)
    {
        out.clear();

        auto        cur    = pos;
        auto        limit  = pos; // pointers must point before this
        bool        jumped = false;
        int         jumps  = 0;
        std::size_t size   = 1; // on the wire: the root's octet, and each label's

        while (true)
        {
            if (cur >= msg.size()) return false;
            const auto length = at(cur);

            if ((length & label_kind_mask) == pointer_kind)
            {
                if (cur + 1 >= msg.size()) return false;
                const std::size_t target = (std::size_t{length & 0x3fU} << 8U) | at(cur + 1);
                if (target >= limit || ++jumps > dns::max_pointers) return false;

                if (!jumped) pos = cur + 2;
                jumped = true;
                limit  = target;
                cur    = target;
                continue;
            }
            if ((length & label_kind_mask) != 0) return false; // reserved (01: RFC 6891's extended labels, gone)

            if (length == 0)
            {
                if (!jumped) pos = cur + 1;
                return true;
            }

            if (!append_label(cur, size, out)) return false;
        }
    }

    // append_label appends the label at cur to out (after a dot, unless it's the first), moving cur past it, and adding
    // its size on the wire to size, which mustn't go over max_name_size.
    bool append_label(std::size_t& cur, std::size_t& size, std::string& out) const
    {
        const std::size_t length = at(cur);
        size += 1 + length;
        if (size > dns::max_name_size || length > msg.size() - cur - 1) return false;

        const auto label = msg.subspan(cur + 1, length);
        if (std::ranges::find(label, std::byte{'.'}) != label.end()) return false;

        if (!out.empty()) out += '.';
        out.append(reinterpret_cast<const char*>(label.data()), label.size());
        cur += 1 + length;
        return true;
    }
};

bool decode_question(reader& in, dns::question& q)
{
    std::uint16_t qtype  = 0;
    std::uint16_t qclass = 0;
    if (!in.name(q.name) || !in.u16(qtype) || !in.u16(qclass)) return false;

    q.type   = static_cast<dns::type>(qtype);
    q.rclass = static_cast<dns::rclass>(qclass);
    return true;
}

// decode_record decodes a record into msg's section, unless it's of a type that's skipped, or the OPT record
// (additional is whether it's the additional section, where an OPT record goes into msg.edns).
bool decode_record(reader& in, dns::message& msg, std::vector<dns::record>& section, bool additional)
{
    dns::record   r;
    std::uint16_t rtype  = 0;
    std::uint16_t rclass = 0;
    std::uint16_t length = 0;
    if (!in.name(r.name) || !in.u16(rtype) || !in.u16(rclass) || !in.u32(r.ttl) || !in.u16(length)) return false;
    if (length > in.left()) return false;

    const auto end = in.pos + length;
    r.rclass       = static_cast<dns::rclass>(rclass);

    switch (static_cast<dns::type>(rtype))
    {
    case dns::type::a:
    {
        if (length != 4) return false;
        r.data = net::ipv4_addr{in.at(in.pos), in.at(in.pos + 1), in.at(in.pos + 2), in.at(in.pos + 3)};
        break;
    }

    case dns::type::aaaa:
    {
        if (length != 16) return false;
        std::array<std::uint8_t, 16> bytes{};
        for (std::size_t i = 0; i < bytes.size(); ++i) bytes.at(i) = in.at(in.pos + i);
        r.data = net::ipv6_addr{bytes};
        break;
    }

    case dns::type::cname:
    {
        dns::cname alias;
        if (!in.name(alias.target) || in.pos != end) return false;
        r.data = std::move(alias);
        break;
    }

    case dns::type::opt:
    {
        // only the additional section's means anything: skipped elsewhere.
        if (!additional)
        {
            in.pos = end;
            return true;
        }
        if (msg.edns.has_value()) return false;

        // the class is the UDP payload size; the TTL the extended RCODE, the version and the flags.
        msg.edns = dns::edns{.udp_size  = rclass,
                             .version   = static_cast<std::uint8_t>(r.ttl >> 16U),
                             .dnssec_ok = (r.ttl & do_bit) != 0};
        msg.header.rcode =
            static_cast<dns::rcode>(((r.ttl >> 24U) << 4U) | static_cast<std::uint16_t>(msg.header.rcode));
        in.pos = end;
        return true;
    }

    default: in.pos = end; return true;
    }

    in.pos = end;
    section.push_back(std::move(r));
    return true;
}

}

namespace net::dns
{

type record::type() const noexcept
{
    if (std::holds_alternative<ipv6_addr>(data)) return type::aaaa;
    if (std::holds_alternative<cname>(data)) return type::cname;
    return type::a;
}

message make_query(std::uint16_t id, std::string_view name, type qtype, bool with_edns)
{
    message msg;
    msg.header.id                = id;
    msg.header.recursion_desired = true;
    msg.questions.push_back({.name = std::string{name}, .type = qtype, .rclass = rclass::in});
    if (with_edns) msg.edns = dns::edns{};
    return msg;
}

std::error_code encode(const message& msg, std::vector<std::byte>& out)
{
    const auto opcode      = static_cast<std::uint8_t>(msg.header.opcode);
    const auto rcode       = static_cast<std::uint16_t>(msg.header.rcode);
    const auto additionals = msg.additionals.size() + (msg.edns.has_value() ? 1 : 0);

    if (opcode > 0x0f || rcode > (msg.edns.has_value() ? 0x0fffU : 0x0fU) || msg.questions.size() > max_count
        || msg.answers.size() > max_count || msg.authorities.size() > max_count || additionals > max_count)
        return std::make_error_code(std::errc::invalid_argument);

    const auto start = out.size();

    encode_header(msg, additionals, out);
    if (!encode_sections(msg, out))
    {
        out.resize(start);
        return std::make_error_code(std::errc::invalid_argument);
    }
    if (msg.edns.has_value()) encode_opt(*msg.edns, msg.header.rcode, out);

    return {};
}

std::expected<message, std::error_code> decode(std::span<const std::byte> in)
{
    const auto bad = std::unexpected(make_error_code(errc::bad_response));

    reader r{.msg = in};

    message       msg;
    std::uint8_t  flags      = 0;
    std::uint8_t  more_flags = 0;
    std::uint16_t questions  = 0;
    std::uint16_t answers    = 0;
    std::uint16_t authority  = 0;
    std::uint16_t additional = 0;
    if (!r.u16(msg.header.id) || !r.u8(flags) || !r.u8(more_flags) || !r.u16(questions) || !r.u16(answers)
        || !r.u16(authority) || !r.u16(additional))
        return bad;

    msg.header.response            = (flags & qr_bit) != 0;
    msg.header.opcode              = static_cast<opcode>((flags >> 3U) & 0x0fU);
    msg.header.authoritative       = (flags & aa_bit) != 0;
    msg.header.truncated           = (flags & tc_bit) != 0;
    msg.header.recursion_desired   = (flags & rd_bit) != 0;
    msg.header.recursion_available = (more_flags & ra_bit) != 0;
    msg.header.authentic_data      = (more_flags & ad_bit) != 0;
    msg.header.rcode               = static_cast<rcode>(more_flags & 0x0fU);

    // the counts are reserved only as far as what's left could hold, so a count past the data doesn't allocate.
    msg.questions.reserve(std::min<std::size_t>(questions, r.left() / min_question_size));
    for (std::uint16_t i = 0; i < questions; ++i)
    {
        if (!decode_question(r, msg.questions.emplace_back())) return bad;
    }

    const std::array sections{
        std::pair{   answers,     &msg.answers},
        std::pair{ authority, &msg.authorities},
        std::pair{additional, &msg.additionals}
    };
    for (const auto& [count, section] : sections)
    {
        section->reserve(std::min<std::size_t>(count, r.left() / min_record_size));
        for (std::uint16_t i = 0; i < count; ++i)
        {
            if (!decode_record(r, msg, *section, section == &msg.additionals)) return bad;
        }
    }

    return msg;
}

std::error_code rcode_error(rcode code) noexcept
{
    switch (code)
    {
    case rcode::no_error: return {};
    case rcode::name_error: return make_error_code(errc::name_not_found);
    case rcode::refused: return make_error_code(errc::refused);
    default: return make_error_code(errc::server_failure);
    }
}

}
