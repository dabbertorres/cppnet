// RFC 6724's destination address selection, as Go's addrselect.go does it.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <vector>

#include <sys/socket.h>
#include <unistd.h>

#include "dns/native.hpp"
#include "ip_addr.hpp"
#include "sockaddr.hpp"
#include "util/defer.hpp"

namespace
{

using bytes16 = std::array<std::uint8_t, 16>;

// as16 is addr's 16 bytes: an IPv4 address mapped into IPv6 (::ffff:a.b.c.d), as RFC 6724's policy table has them.
bytes16 as16(const net::ip_addr& addr) noexcept
{
    bytes16     out{};
    const auto* data = static_cast<const std::uint8_t*>(addr);
    if (addr.is_ipv4())
    {
        out[10] = 0xff;
        out[11] = 0xff;
        std::copy_n(data, 4, out.begin() + 12);
    }
    else
    {
        std::copy_n(data, out.size(), out.begin());
    }
    return out;
}

// matches is whether addr is in the prefix of bits bits.
constexpr bool matches(const bytes16& prefix, unsigned bits, const bytes16& addr) noexcept
{
    for (std::size_t i = 0; bits > 0; ++i)
    {
        const unsigned take = bits >= 8 ? 8 : bits;
        const auto     mask = static_cast<std::uint8_t>(0xffU << (8 - take));
        if ((prefix[i] & mask) != (addr[i] & mask)) return false;
        bits -= take;
    }
    return true;
}

struct policy
{
    bytes16      prefix;
    unsigned     bits;
    std::uint8_t precedence;
    std::uint8_t label;
};

// policy_table is RFC 6724's default policy table (section 2.1), longest prefixes first, so the first match is the
// longest.
constexpr std::array<policy, 9> policy_table{
    {
     {.prefix = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}, .bits = 128, .precedence = 50, .label = 0}, // ::1
        {.prefix     = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff},
         .bits       = 96,
         .precedence = 35,
         .label      = 4},                                                  // ::ffff:0:0/96
        {.prefix = {}, .bits = 96, .precedence = 1, .label = 3},            // ::/96
        {.prefix = {0x20, 0x01}, .bits = 32, .precedence = 5, .label = 5},  // 2001::/32
        {.prefix = {0x20, 0x02}, .bits = 16, .precedence = 30, .label = 2}, // 2002::/16
        {.prefix = {0x3f, 0xfe}, .bits = 16, .precedence = 1, .label = 12}, // 3ffe::/16
        {.prefix = {0xfe, 0xc0}, .bits = 10, .precedence = 1, .label = 11}, // fec0::/10
        {.prefix = {0xfc}, .bits = 7, .precedence = 3, .label = 13},        // fc00::/7
        {.prefix = {}, .bits = 0, .precedence = 40, .label = 1},            // ::/0
    }
};

// scope is an address's scope (RFC 4007's, as RFC 6724 uses them).
enum class scope : std::uint8_t
{
    link_local = 0x2,
    site_local = 0x5,
    global     = 0xe,
};

scope scope_of(const net::ip_addr& addr) noexcept
{
    const auto bytes = as16(addr);

    if (addr.is_ipv4())
    {
        // loopback (127/8) and link-local (169.254/16)
        if (bytes[12] == 127 || (bytes[12] == 169 && bytes[13] == 254)) return scope::link_local;
        return scope::global;
    }

    constexpr bytes16 loopback{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    if (bytes == loopback) return scope::link_local;
    if (bytes[0] == 0xfe && (bytes[1] & 0xc0) == 0x80) return scope::link_local; // fe80::/10
    if (bytes[0] == 0xff) return static_cast<scope>(bytes[1] & 0x0f);            // multicast: its own
    if (bytes[0] == 0xfe && (bytes[1] & 0xc0) == 0xc0) return scope::site_local; // fec0::/10
    return scope::global;
}

struct attributes
{
    scope        sc         = scope::global;
    std::uint8_t precedence = 0;
    std::uint8_t label      = 0;
};

attributes attributes_of(const net::ip_addr& addr) noexcept
{
    const auto bytes = as16(addr);

    // NOTE: the last, ::/0, matches everything.
    const policy* match = &policy_table.back();
    for (const auto& p : policy_table)
    {
        if (matches(p.prefix, p.bits, bytes))
        {
            match = &p;
            break;
        }
    }

    return {.sc = scope_of(addr), .precedence = match->precedence, .label = match->label};
}

// common_prefix_len is how many leading bits a and b (both IPv6) share, in their first 64 (the prefix, as Go counts
// it).
unsigned common_prefix_len(const net::ip_addr& a, const net::ip_addr& b) noexcept
{
    const auto lhs = as16(a);
    const auto rhs = as16(b);

    unsigned common = 0;
    for (std::size_t i = 0; i < 8; ++i)
    {
        const auto diff = static_cast<std::uint8_t>(lhs[i] ^ rhs[i]);
        if (diff == 0)
        {
            common += 8;
            continue;
        }
        for (unsigned bit = 0x80; (diff & bit) == 0; bit >>= 1U) ++common;
        break;
    }
    return common;
}

struct entry
{
    net::ip_addr                addr;
    std::optional<net::ip_addr> source;
    attributes                  attr;
    attributes                  source_attr;
};

// before is whether a goes before b: RFC 6724's rules, in their order, those the OS doesn't tell us about left out.
bool before(const entry& a, const entry& b) noexcept
{
    // Rule 1: avoid unusable destinations.
    if (a.source.has_value() != b.source.has_value()) return a.source.has_value();
    if (!a.source.has_value()) return false;

    // Rule 2: prefer matching scope.
    const bool a_scope = a.attr.sc == a.source_attr.sc;
    const bool b_scope = b.attr.sc == b.source_attr.sc;
    if (a_scope != b_scope) return a_scope;

    // Rules 3 (avoid deprecated addresses) and 4 (prefer home addresses): not known.

    // Rule 5: prefer matching label.
    const bool a_label = a.attr.label == a.source_attr.label;
    const bool b_label = b.attr.label == b.source_attr.label;
    if (a_label != b_label) return a_label;

    // Rule 6: prefer higher precedence.
    if (a.attr.precedence != b.attr.precedence) return a.attr.precedence > b.attr.precedence;

    // Rule 7 (prefer native transport): not known.

    // Rule 8: prefer smaller scope.
    if (a.attr.sc != b.attr.sc) return a.attr.sc < b.attr.sc;

    // Rule 9: use the longest matching prefix, for IPv6 only (as Go does, after RFC 6724's errata: IPv4's would prefer
    // addresses in the same /8, which says nothing about their distance).
    if (a.addr.is_ipv6() && b.addr.is_ipv6())
    {
        const auto a_common = common_prefix_len(*a.source, a.addr);
        const auto b_common = common_prefix_len(*b.source, b.addr);
        if (a_common != b_common) return a_common > b_common;
    }

    // Rule 10: otherwise, leave the order unchanged.
    return false;
}

// source_for is the source address the OS would use to reach addr, if it can.
std::optional<net::ip_addr> source_for(const net::ip_addr& addr) noexcept
{
    const auto to = net::detail::to_sockaddr(addr, 53);

    const int fd = ::socket(to.family(), SOCK_DGRAM, 0);
    if (fd == -1) return std::nullopt;
    const net::util::defer close{[fd] noexcept { ::close(fd); }};

    // NOTE: a datagram socket connects at once, choosing its source address: nothing is sent.
    if (::connect(fd, to.get(), to.size) != 0) return std::nullopt;

    net::detail::socket_address local;
    local.size = sizeof(local.storage);
    if (::getsockname(fd, local.get(), &local.size) != 0) return std::nullopt;

    auto found = net::detail::from_sockaddr(local);
    if (!found.has_value()) return std::nullopt;
    return found->addr;
}

}

namespace net::dns::detail
{

void sort_by_rfc6724(std::vector<ip_addr>& addrs, std::span<const std::optional<ip_addr>> sources)
{
    std::vector<entry> entries;
    entries.reserve(addrs.size());
    for (std::size_t i = 0; i < addrs.size(); ++i)
    {
        const auto& source = sources[i];
        entries.push_back({
            .addr        = addrs[i],
            .source      = source,
            .attr        = attributes_of(addrs[i]),
            .source_attr = source.has_value() ? attributes_of(*source) : attributes{},
        });
    }

    std::ranges::stable_sort(entries, before);

    for (std::size_t i = 0; i < addrs.size(); ++i) addrs[i] = entries[i].addr;
}

void sort_by_rfc6724(std::vector<ip_addr>& addrs)
{
    if (addrs.size() < 2) return;

    std::vector<std::optional<ip_addr>> sources;
    sources.reserve(addrs.size());
    for (const auto& addr : addrs) sources.push_back(source_for(addr));

    sort_by_rfc6724(addrs, sources);
}

}
