#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>
#include <vector>

#include "ipv4_addr.hpp"
#include "ipv6_addr.hpp"

// DNS messages (RFC 1035, section 4, and EDNS(0)'s OPT record, RFC 6891): what the native resolver is built from, not
// for applications (see docs/plans/dns-resolver.md). Only what looking up addresses needs: A, AAAA and CNAME records'
// data are decoded, and other records are skipped.
//
// Names are text: their labels joined by dots, without a final dot ("" is the root), in the case they were sent in. A
// label may hold any octet but a dot (which couldn't be told from the dots between labels; host names have none).
namespace net::dns
{

// header_size is a message's header's size, before its sections.
inline constexpr std::size_t header_size = 12;

// max_label_size and max_name_size are RFC 1035's limits (section 2.3.4): a label's octets, and a name's on the wire,
// uncompressed (its labels with their length octets, and the root's).
inline constexpr std::size_t max_label_size = 63;
inline constexpr std::size_t max_name_size  = 255;

// max_pointers is the most compression pointers decoding a name follows (as Go's dnsmessage): more than encoders
// chain, and few enough that a message of nothing but pointers decodes quickly. (Each must also point before the last,
// so they can't loop.)
inline constexpr int max_pointers = 10;

// edns_udp_size is the UDP payload size queries offer with EDNS(0): DNS Flag Day 2020's, as Go's resolver sends, small
// enough not to be fragmented.
inline constexpr std::uint16_t edns_udp_size = 1'232;

// opcode is the kind of query: 4 bits. Others are kept as their values.
enum class opcode : std::uint8_t
{
    query = 0,
};

// rcode is a response's code: 4 bits in the header, and 12 with an OPT record's 8 more (RFC 6891, section 6.1.3).
// Others are kept as their values.
// NOLINTNEXTLINE(performance-enum-size): 12 bits.
enum class rcode : std::uint16_t
{
    no_error        = 0,
    format_error    = 1,  // FORMERR
    server_failure  = 2,  // SERVFAIL
    name_error      = 3,  // NXDOMAIN
    not_implemented = 4,  // NOTIMP
    refused         = 5,  // REFUSED
    bad_version     = 16, // BADVERS: the OPT record's version isn't supported (needs EDNS's bits)
};

// type is a record's (or a question's) type. Others are kept as their values.
// NOLINTNEXTLINE(performance-enum-size): a 16-bit field on the wire.
enum class type : std::uint16_t
{
    a     = 1,
    cname = 5,
    aaaa  = 28,
    opt   = 41, // EDNS(0)'s pseudo-record, which message::edns holds
};

// rclass is a record's (or a question's) class. Others are kept as their values.
// NOLINTNEXTLINE(performance-enum-size): a 16-bit field on the wire.
enum class rclass : std::uint16_t
{
    in = 1,
};

// header is a message's header, but for the sections' counts (which encoding takes from the sections).
struct header
{
    std::uint16_t id                  = 0;
    bool          response            = false;                // QR
    dns::opcode   opcode              = dns::opcode::query;   // OPCODE: at most 15
    bool          authoritative       = false;                // AA
    bool          truncated           = false;                // TC
    bool          recursion_desired   = false;                // RD
    bool          recursion_available = false;                // RA
    bool          authentic_data      = false;                // AD (RFC 4035, section 3.2.3): resolv.conf's trust-ad
    dns::rcode    rcode               = dns::rcode::no_error; // RCODE: at most 15 without EDNS(0), and 4095 with it

    bool operator==(const header&) const = default;
};

struct question
{
    std::string name;
    dns::type   type   = dns::type::a;
    dns::rclass rclass = dns::rclass::in;

    bool operator==(const question&) const = default;
};

// cname is a CNAME record's data: the name the record's name is an alias of.
struct cname
{
    std::string target;

    bool operator==(const cname&) const = default;
};

// record is a resource record of one of the types decoded: its data's alternative is its type (A, AAAA or CNAME).
struct record
{
    std::string                               name;
    dns::rclass                               rclass = dns::rclass::in;
    std::uint32_t                             ttl    = 0;
    std::variant<ipv4_addr, ipv6_addr, cname> data;

    // type is the record's type, by its data's alternative.
    [[nodiscard]] dns::type type() const noexcept;

    bool operator==(const record&) const = default;
};

// edns is what an OPT record says (RFC 6891, section 6.1.3), without its options (which are skipped).
struct edns
{
    // the largest UDP payload the sender can take.
    std::uint16_t udp_size  = edns_udp_size;
    std::uint8_t  version   = 0;
    bool          dnssec_ok = false; // DO

    bool operator==(const edns&) const = default;
};

struct message
{
    dns::header              header{};
    std::vector<question>    questions;
    std::vector<record>      answers;
    std::vector<record>      authorities;
    std::vector<record>      additionals; // but the OPT record, which is edns
    std::optional<dns::edns> edns;

    bool operator==(const message&) const = default;
};

// make_query is a query of name's records of type qtype (class IN), asking for recursion, as a stub resolver's are,
// with an OPT record offering edns_udp_size (no options, and DO clear) unless with_edns is false.
[[nodiscard]] message make_query(std::uint16_t id, std::string_view name, type qtype, bool with_edns = true);

// encode appends msg to out, without compressing its names. It fails with std::errc::invalid_argument, leaving out as
// it was, if:
// - a name isn't valid: labels of 1 to 63 octets but dots, joined by dots, coming to at most max_name_size on the wire.
//   (A name may end in a dot, which is dropped, and "." is the root, as "" is.)
// - the opcode is over 15, or the rcode over 15 without edns, or 4095 with it.
// - a section has more than 65535 entries (the additional section counting the OPT record).
[[nodiscard]] std::error_code encode(const message& msg, std::vector<std::byte>& out);

// decode decodes a message: a whole one, as a UDP datagram or a TCP message (without its length) holds it. Records of
// types other than A, AAAA and CNAME are skipped, as are the OPT record's options; an OPT record's extended RCODE is
// joined to the header's. Anything after the last record is ignored. It fails with errc::bad_response if the message is
// malformed: shorter than its header, or than its counts or a record's RDLENGTH say; a name that's over max_name_size
// (after its pointers are followed), that has a label with a dot, or reserved label types; a compression pointer
// that doesn't point before where the name has been read from so far (so pointers can't loop), or more than
// max_pointers in a name; an A or AAAA record whose data isn't an address's size, or a CNAME's whose isn't one name;
// more than one OPT record.
//
// It makes no checks of what the message means (whether it's a response, its question, its RCODE): the resolver's.
[[nodiscard]] std::expected<message, std::error_code> decode(std::span<const std::byte> in);

// rcode_error is the error a response's code means: none for no_error, errc::name_not_found for NXDOMAIN,
// errc::refused for REFUSED, and errc::server_failure for the others (SERVFAIL, FORMERR, NOTIMP, ...: nothing
// usable).
[[nodiscard]] std::error_code rcode_error(rcode code) noexcept;

}
