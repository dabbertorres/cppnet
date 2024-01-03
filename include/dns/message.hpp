#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "ipv4_addr.hpp"
#include "ipv6_addr.hpp"

namespace net::dns
{

// NOTE: all values must be transmitted in network order

enum class message_type : std::uint8_t
{
    query = 0,
    reply = 1,
};

enum class opcode : std::uint8_t
{
    query         = 0,
    inverse_query = 1,
    status        = 2,
    // 3 - 15 reserved
};

enum class response : std::uint8_t
{
    no_error           = 0,
    format_error       = 1,
    server_failure     = 2,
    name_error         = 3,
    nonexistent_domain = name_error, // alias
    nxdomain           = name_error, // alias
    not_implemented    = 4,
    refused            = 5,
    // 6 - 15 reserved
};

enum class resource_record_type : std::uint16_t
{
    A     = 1,
    NS    = 2,
    MD    = 3,
    MF    = 4,
    CNAME = 5,
    SOA   = 6,
    MB    = 7,
    MG    = 8,
    MR    = 9,
    NULL_ = 10,
    WKS   = 11,
    PTR   = 12,
    HINFO = 13,
    MINFO = 14,
    MX    = 15,
    TXT   = 16,

    // only valid for queries (QTYPEs)
    AXFR  = 252,
    MAILB = 253,
    MAILA = 254,
    ALL   = 255, // "*"
};

enum class class_type : std::uint16_t
{
    IN  = 1,
    CS  = 2,
    CH  = 3,
    HS  = 4,
    ANY = 255, // "*"
};

struct message_header
{
    std::uint16_t message_id;               // "ID"
    message_type  type                 : 1; // "QR"
    opcode        opcode               : 4; // "OPCODE"
    bool          authoritative_answer : 1; // "AA"
    bool          truncation           : 1; // "TC"
    bool          recursion_desired    : 1; // "RD"
    bool          recursion_available  : 1; // "RA"
    std::uint8_t  zero                 : 3; // "Z", reserved for future use
    response      response_code        : 4; // "RCODE"
    std::uint16_t num_questions;            // "QDCOUNT"
    std::uint16_t num_answers;              // "ANCOUNT"
    std::uint16_t num_name_server;          // "NSCOUNT"
    std::uint16_t num_additional;           // "ARCOUNT"
};

// data types of interest:
//
// * <label>
//   One octet length field followed by that number of octets.
//   The high order two bits of every length octet must be zero,
//   and the remaining six bits of the length field limit the label
//   to 63 octets or less.
// * <domain-name>
//   A series of labels, terminated by a label with zero length.
// * <character-string>
//   A single length octet followed by that number of characters,
//   and should be treated as binary information.
//   Up to 256 characters in length (including the length octet).

// Size limits:
//
// * labels:       63 octets or less.
// * names:        255 octets or less.
// * TTL:          positive values of a signed 32 bit number.
// * UDP messages: 512 octets or less

struct cname_rdata
{
    std::string cname; // domain-name
};

struct hinfo_rdata
{
    std::string cpu; // character-string
    std::string os;  // character-string
};

struct mb_rdata
{
    std::string madname; // domain-name
};

struct md_rdata
{
    std::string madname; // domain-name
};

struct mf_rdata
{
    std::string madname; // domain-name
};

struct mg_rdata
{
    std::string mgmname; // domain-name
};

struct minfo_rdata
{
    std::string rmailbx; // domain-name
    std::string emailbx; // domain-name
};

struct mr_rdata
{
    std::string newname; // domain-name
};

struct mx_rdata
{
    std::int16_t preference;
    std::string  exchange; // domain-name
};

struct null_rdata
{
    std::string anything; // anything, 65535 octets or less
};

struct ns_rdata
{
    std::string nsdname; // domain-name
};

struct ptr_rdata
{
    std::string ptrdname; // domain-name
};

struct soa_rdata
{
    std::string   mname;  // domain-name
    std::string   rname;  // domain-name
    std::uint32_t serial; // "...wraps and should be compared using sequence space arithmetic"
    std::int32_t  refresh;
    std::int32_t  retry;
    std::int32_t  expire;
    std::uint32_t minimum;
};

struct txt_rdata
{
    std::vector<std::string> data; // one-or-more character-string
};

struct a_rdata
{
    ipv4_addr address;
};

struct aaaa_rdata
{
    ipv6_addr address;
};

struct wks_rdata
{
    ipv4_addr                 address;
    std::uint8_t              protocol;
    std::vector<std::uint8_t> bitmap; // ???
};

// NOTE: should be interpreted based off of resource_record's type field.
// clang-format off
using resource_record_data = std::variant<
    cname_rdata,
    hinfo_rdata,
    mb_rdata,
    md_rdata,
    mf_rdata,
    mg_rdata,
    minfo_rdata,
    mr_rdata,
    mx_rdata,
    null_rdata,
    ns_rdata,
    ptr_rdata,
    soa_rdata,
    txt_rdata,
    a_rdata,
    aaaa_rdata,
    wks_rdata
>;
// clang-format on

struct question
{
    std::string          qname; // domain-name
    resource_record_type qtype;
    class_type           qclass;
};

struct resource_record
{
    // implicit 1-byte field containing length of name.
    std::string          name;         // "NAME"
    resource_record_type type;         // "TYPE"
    class_type           class_code;   // "CLASS"
    std::uint32_t        ttl;          // "TTL"
    std::uint16_t        rdata_length; // "RDLENGTH"
    resource_record_data rdata;
};

// full layout of a DNS message
//
// Compression:
//   An entire domain-name or a list of labels at the end of a domain name
//   is replaced with a pointer to a prior occurance of the same name.
//
//   The pointer takes the form of a two octet sequence:
//
//   +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
//   | 1| 1|                OFFSET                   |
//   +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
//
//   The first two bits are ones. This allows a pointer to be distinguished
//   from a label, since the label must begin with two zero bits because
//   labels are restricted to 63 octets or less (The 10 and 01 combos
//   are reserved for future use).
//   The OFFSET field specifies an offest from the start of the message.
//
//   If a domain name is contained in a part of the message subject to a
//   length field (such as the RDATA section of an RR), and compression
//   is used, the length of the compressed name is used in the length
//   calculation, rather than the length of the expanded name.
struct message
{
    message_header               header{};
    std::vector<question>        questions;
    std::vector<resource_record> answers;
    std::vector<resource_record> authorities;
    std::vector<resource_record> additional;
};

// transport specific details:
//
// UDP:
//   Messages carried by UDP are restricted to 512 bytes (not counting
//   IP or UDP headers). Longer messages are truncated and the TC bit
//   is set in the header.
//
//   UDP is not acceptable for zone transfers, but is the recommended
//   method for standard queries in the Internet.
//
//   Recommendations:
//   * The client should try other servers and server addresses before
//     repeating a query to a specific address of a server.
//   * The retransmission interval should be based on prior statistics
//     if possible. Depending on connection to servers, the minimum
//     retransmission interval should be 2-5 seconds.
//
// TCP:
//   Messages sent over TCP are prefixed with a two byte length field
//   which gives the message length (excluding the two byte length
//   field). This allows for the complete assembly of a message before
//   parsing.

// TO BE CONTINUED: https://datatracker.ietf.org/doc/html/rfc1035#section-7

}
