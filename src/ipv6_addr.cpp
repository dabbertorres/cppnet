#include "ipv6_addr.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

#include "ipv4_addr.hpp"
#include "util/string_util.hpp"

namespace
{

using groups_t = std::array<std::uint16_t, 8>;

// parse_groups parses str's colon-separated groups into out, returning how many there were. If allow_ipv4, the last
// group may be an IPv4 address, which is two.
// NOLINTNEXTLINE(bugprone-exception-escape): split_string's substr positions are in range.
std::optional<std::size_t> parse_groups(std::string_view str, std::span<std::uint16_t> out, bool allow_ipv4) noexcept
{
    if (str.empty()) return 0;

    std::size_t parsed = 0;
    for (auto part : net::util::split_string(str, ':'))
    {
        const bool last = part.data() + part.size() == str.data() + str.size();

        if (allow_ipv4 && last && part.contains('.'))
        {
            auto v4 = net::ipv4_addr::parse(part);
            if (!v4 || out.size() - parsed < 2) return std::nullopt;

            const auto bits = static_cast<std::uint32_t>(*v4);
            out[parsed++]   = static_cast<std::uint16_t>(bits >> 16);
            out[parsed++]   = static_cast<std::uint16_t>(bits);
            break;
        }

        if (parsed == out.size() || part.empty() || part.size() > 4) return std::nullopt;

        auto [end, err] = std::from_chars(part.data(), part.data() + part.size(), out[parsed++], 16);
        if (err != std::errc{} || end != part.data() + part.size()) return std::nullopt;
    }

    return parsed;
}
}

namespace net
{

// NOLINTNEXTLINE(bugprone-exception-escape): substr's positions are in range (from find).
std::expected<ipv6_addr, std::error_code> ipv6_addr::parse(std::string_view str) noexcept
{
    groups_t groups{};

    // without "::", all 8 groups are given.
    const auto gap = str.find("::");
    if (gap == std::string_view::npos)
    {
        auto parsed = parse_groups(str, groups, true);
        if (!parsed || *parsed != groups.size())
            return std::unexpected(std::make_error_code(std::errc::invalid_argument));
        return ipv6_addr(groups);
    }

    // otherwise "::" stands for at least one group of zeros, between those before it and those after it.
    const auto before = str.substr(0, gap);
    const auto after  = str.substr(gap + 2);
    if (after.contains("::")) return std::unexpected(std::make_error_code(std::errc::invalid_argument));

    groups_t after_groups{};

    auto parsed_before = parse_groups(before, groups, false);
    auto parsed_after  = parse_groups(after, after_groups, true);
    if (!parsed_before || !parsed_after || *parsed_before + *parsed_after >= groups.size())
        return std::unexpected(std::make_error_code(std::errc::invalid_argument));

    std::copy_n(after_groups.begin(), *parsed_after, groups.end() - static_cast<std::ptrdiff_t>(*parsed_after));
    return ipv6_addr(groups);
}

// to_string formats the address as RFC 5952 recommends: lowercase, without leading zeros, and with the longest run of
// two or more zero groups (the first, if there's a tie) shortened to "::".
[[nodiscard]] std::string ipv6_addr::to_string() const
{
    groups_t groups{};
    for (std::size_t i = 0; i < groups.size(); ++i)
        groups[i] = static_cast<std::uint16_t>((data[2 * i] << 8) | data[(2 * i) + 1]);

    std::size_t gap_start = groups.size();
    std::size_t gap_size  = 1; // so that a single zero group isn't shortened.
    for (std::size_t i = 0; i < groups.size();)
    {
        if (groups[i] != 0)
        {
            ++i;
            continue;
        }

        auto end = i;
        while (end < groups.size() && groups[end] == 0) ++end;

        if (end - i > gap_size)
        {
            gap_start = i;
            gap_size  = end - i;
        }

        i = end;
    }

    std::string out;
    out.reserve(39); // the longest address: 8 groups of 4, and 7 colons.

    for (std::size_t i = 0; i < groups.size(); ++i)
    {
        if (i == gap_start)
        {
            out += "::";
            i += gap_size - 1;
            continue;
        }

        if (!out.empty() && !out.ends_with(':')) out += ':';

        std::array<char, 4> buf{};
        auto*               end = std::to_chars(buf.begin(), buf.end(), groups[i], 16).ptr;
        out.append(buf.begin(), end);
    }

    return out;
}
}
