#include "url.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

std::optional<int> hex_digit_value(char c) noexcept
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return std::nullopt;
}

using namespace std::string_view_literals;

using net::url;
using net::url_parse_failure;
using net::url_parse_state;

constexpr bool is_alpha(char c) noexcept { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
constexpr bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }

constexpr bool is_unreserved(char c) noexcept
{
    return is_alpha(c) || is_digit(c) || c == '-' || c == '.' || c == '_' || c == '~';
}

constexpr bool is_hex_digit(char c) noexcept
{
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

// is_valid_zone checks a percent-encoded zone ID, including its "%25" prefix.
constexpr bool is_valid_zone(std::string_view zone) noexcept
{
    if (!zone.starts_with("%25"sv) || zone.size() == 3) return false;

    for (std::size_t i = 3; i < zone.size(); ++i)
    {
        if (zone[i] == '%')
        {
            if (zone.size() - i < 3 || !is_hex_digit(zone[i + 1]) || !is_hex_digit(zone[i + 2])) return false;
            i += 2;
        }
        else if (!is_unreserved(zone[i]))
        {
            return false;
        }
    }

    return true;
}

constexpr bool is_sub_delim(char c) noexcept { return "!$&'()*+,;="sv.contains(c); }
constexpr bool is_scheme_char(char c) noexcept { return is_alpha(c) || is_digit(c) || c == '+' || c == '-' || c == '.'; }

// Characters that may appear unencoded in each part (RFC 3986).
constexpr bool is_userinfo_char(char c) noexcept { return is_unreserved(c) || is_sub_delim(c); }
constexpr bool is_host_char(char c) noexcept { return is_unreserved(c) || is_sub_delim(c); }
constexpr bool is_path_char(char c) noexcept
{
    return is_unreserved(c) || is_sub_delim(c) || c == ':' || c == '@' || c == '/';
}
constexpr bool is_fragment_char(char c) noexcept { return is_path_char(c) || c == '?'; }

// The WHATWG URL standard's forbidden domain code points (in ASCII): a host containing these, once decoded, could be
// mistaken for another part of a URL, or end a header line.
constexpr bool is_forbidden_host_char(char c) noexcept
{
    auto byte = static_cast<unsigned char>(c);
    return byte <= 0x20 || byte == 0x7F || "#%/:<>?@[\\]^|"sv.contains(c);
}

template<typename Allowed>
void percent_encode(std::string& out, std::string_view str, Allowed allowed)
{
    static constexpr auto hex = "0123456789ABCDEF"sv;

    for (char c : str)
    {
        if (allowed(c))
        {
            out += c;
            continue;
        }

        auto byte = static_cast<unsigned char>(c);
        out += '%';
        out += hex[byte >> 4];
        out += hex[byte & 0x0F];
    }
}

void append_query(std::string& out, const url::query_values& query)
{
    // sorted, so the output doesn't depend on the map's order.
    std::vector<const url::query_values::value_type*> params;
    params.reserve(query.size());
    for (const auto& param : query) params.push_back(&param);
    std::ranges::sort(params, {}, [](const auto* param) -> const std::string& { return param->first; });

    char separator = '?';
    for (const auto* param : params)
    {
        const auto& [key, values] = *param;

        if (values.empty())
        {
            out += std::exchange(separator, '&');
            percent_encode(out, key, is_unreserved);
        }

        for (const auto& value : values)
        {
            out += std::exchange(separator, '&');
            percent_encode(out, key, is_unreserved);
            out += '=';
            percent_encode(out, value, is_unreserved);
        }
    }
}

std::optional<url_parse_failure> parse_port(std::string_view port, std::size_t offset, url& out)
{
    // RFC 3986 allows an empty port.
    unsigned value = 0;
    if (port.size() > 5 || !std::ranges::all_of(port, is_digit)
        || (!port.empty() && (std::from_chars(port.data(), port.data() + port.size(), value), value > 65'535)))
        return url_parse_failure{.failed_at = url_parse_state::port, .index = offset};

    out.port = port;
    return std::nullopt;
}

// parse_authority parses "[userinfo@]host[:port]", where offset is where it starts in the URL (for errors).
std::optional<url_parse_failure> parse_authority(std::string_view authority, std::size_t offset, url& out)
{
    // userinfo can't contain an '@' unencoded, but be lenient (like the WHATWG URL standard) and split at the last one.
    if (auto at = authority.rfind('@'); at != std::string_view::npos)
    {
        auto userinfo = authority.substr(0, at);
        auto colon    = userinfo.find(':');

        out.userinfo.username = url::decode(userinfo.substr(0, colon));
        if (colon != std::string_view::npos) out.userinfo.password = url::decode(userinfo.substr(colon + 1));

        authority = authority.substr(at + 1);
        offset += at + 1;
    }

    if (authority.starts_with('['))
    {
        // an IP literal, e.g. "[::1]:8080"
        auto end = authority.find(']');
        if (end == std::string_view::npos || end == 1)
            return url_parse_failure{.failed_at = url_parse_state::host, .index = offset};

        // hex digits, ':' and '.' (IPv6, possibly ending in IPv4), and an optional zone ID after "%25" (RFC 6874).
        auto literal = authority.substr(1, end - 1);
        auto zone    = literal.find('%');
        auto address = literal.substr(0, zone);
        // (an IPv6 address always has at least two colons, which is also how build() knows to bracket it.)
        if (std::ranges::count(address, ':') < 2
            || !std::ranges::all_of(address, [](char c) { return is_hex_digit(c) || c == ':' || c == '.'; })
            || (zone != std::string_view::npos && !is_valid_zone(literal.substr(zone))))
            return url_parse_failure{.failed_at = url_parse_state::host, .index = offset};

        out.host = literal;

        auto rest = authority.substr(end + 1);
        if (rest.empty()) return std::nullopt;
        if (rest.front() != ':') return url_parse_failure{.failed_at = url_parse_state::port, .index = offset + end + 1};

        return parse_port(rest.substr(1), offset + end + 2, out);
    }

    auto colon = authority.find(':');

    out.host = url::decode(authority.substr(0, colon));
    if (std::ranges::any_of(out.host, is_forbidden_host_char))
        return url_parse_failure{.failed_at = url_parse_state::host, .index = offset};

    if (colon == std::string_view::npos) return std::nullopt;
    return parse_port(authority.substr(colon + 1), offset + colon + 1, out);
}

url::parse_result parse_url(std::string_view input)
{
    url         out;
    std::size_t pos = 0;

    // scheme: ALPHA *( ALPHA / DIGIT / "+" / "-" / "." ) ":"
    if (auto colon = input.find(':');
        colon != std::string_view::npos && colon > 0 && is_alpha(input.front())
        && std::ranges::all_of(input.substr(0, colon), is_scheme_char))
    {
        out.scheme = input.substr(0, colon);
        pos        = colon + 1;
    }

    auto rest = input.substr(pos);

    const bool has_authority = rest.starts_with("//"sv);
    const bool starts_with_host =
        !has_authority && out.scheme.empty() && !rest.empty() && !"/?#"sv.contains(rest.front());

    if (has_authority || starts_with_host)
    {
        auto authority_start = has_authority ? pos + 2 : pos;
        auto authority_end   = input.find_first_of("/?#"sv, authority_start);
        if (authority_end == std::string_view::npos) authority_end = input.size();

        if (auto err = parse_authority(input.substr(authority_start, authority_end - authority_start), authority_start, out))
            return std::unexpected(*err);

        pos = authority_end;
    }

    auto path_end = input.find_first_of("?#"sv, pos);
    if (path_end == std::string_view::npos) path_end = input.size();
    out.path = url::decode(input.substr(pos, path_end - pos));
    pos      = path_end;

    if (pos < input.size() && input[pos] == '?')
    {
        auto query_end = input.find('#', pos);
        if (query_end == std::string_view::npos) query_end = input.size();
        out.query = url::parse_query(input.substr(pos + 1, query_end - pos - 1));
        pos       = query_end;
    }

    if (pos < input.size() && input[pos] == '#') out.fragment = url::decode(input.substr(pos + 1));

    return {std::move(out)};
}

}

namespace net
{

bool url::user_info::empty() const { return username.empty() && password.empty(); }

std::string url::encode(std::string_view str, std::string_view reserved) noexcept
{
    std::string out;
    out.reserve(str.size());

    percent_encode(out,
                   str,
                   [&](char c)
                   {
                       auto byte = static_cast<unsigned char>(c);
                       return byte > 0x20 && byte < 0x7F && !reserved.contains(c);
                   });

    return out;
}

std::string url::decode(std::string_view str) noexcept
{
    std::string out;
    out.reserve(str.size());
    decode_append(out, str);
    return out;
}

void url::decode_append(std::string& out, std::string_view str) noexcept
{
    // Follows the WHATWG URL standard's percent-decode: a '%' that isn't followed by two hex digits is kept as it is.
    for (std::size_t i = 0; i < str.size(); ++i)
    {
        if (str[i] == '%' && str.size() - i > 2)
        {
            auto high = hex_digit_value(str[i + 1]);
            auto low  = hex_digit_value(str[i + 2]);

            if (high && low)
            {
                out.push_back(static_cast<char>((*high << 4) | *low));
                i += 2;
                continue;
            }
        }

        out.push_back(str[i]);
    }
}

url::query_values url::parse_query(std::string_view query)
{
    const auto decode_form = [](std::string_view str)
    {
        std::string plus_as_space{str};
        std::ranges::replace(plus_as_space, '+', ' ');
        return decode(plus_as_space);
    };

    query_values values;

    while (!query.empty())
    {
        auto pair = query.substr(0, query.find('&'));
        query.remove_prefix(std::min(pair.size() + 1, query.size()));

        if (pair.empty()) continue;

        auto  eq     = pair.find('=');
        auto& values_for_key = values[decode_form(pair.substr(0, eq))];
        if (eq != std::string_view::npos) values_for_key.push_back(decode_form(pair.substr(eq + 1)));
    }

    return values;
}

url::parse_result url::parse(const char* s) noexcept { return parse(std::string_view(s)); }

url::parse_result url::parse(const std::string& s) noexcept { return parse(std::string_view{s}); }

url::parse_result url::parse(std::string_view s) noexcept
{
    try
    {
        return parse_url(s);
    }
    catch (...)
    {
        // only allocation can fail.
        return std::unexpected(url_parse_failure{.failed_at = url_parse_state::scheme, .index = 0});
    }
}

std::string url::build() const noexcept
{
    std::string out;

    if (!scheme.empty())
    {
        out += scheme;
        out += ':';
    }

    // NOTE: "//" is also needed when the path starts with "//", which would otherwise be taken for the authority.
    if (!host.empty() || !userinfo.empty() || !port.empty() || path.starts_with("//"))
    {
        out += "//";

        if (!userinfo.empty())
        {
            percent_encode(out, userinfo.username, is_userinfo_char);
            if (!userinfo.password.empty())
            {
                out += ':';
                percent_encode(out, userinfo.password, is_userinfo_char);
            }
            out += '@';
        }

        if (host.contains(':'))
        {
            out += '[';
            out += host;
            out += ']';
        }
        else
        {
            percent_encode(out, host, is_host_char);
        }

        if (!port.empty())
        {
            out += ':';
            out += port;
        }

        // with an authority, the path must be empty or absolute.
        if (!path.empty() && !path.starts_with('/')) out += '/';
    }

    percent_encode(out, path, is_path_char);
    append_query(out, query);

    if (!fragment.empty())
    {
        out += '#';
        percent_encode(out, fragment, is_fragment_char);
    }

    return out;
}

std::string url::origin_form() const
{
    std::string out;

    if (!path.starts_with('/')) out += '/';
    percent_encode(out, path, is_path_char);
    append_query(out, query);

    return out;
}

bool operator==(const url::user_info& lhs, const url::user_info& rhs) noexcept
{
    return lhs.username == rhs.username && lhs.password == rhs.password;
}

bool operator==(const url& lhs, const url& rhs) noexcept
{
    // clang-format off
    return lhs.scheme   == rhs.scheme
        && lhs.userinfo == rhs.userinfo
        && lhs.host     == rhs.host
        && lhs.port     == rhs.port
        && lhs.path     == rhs.path
        && lhs.query    == rhs.query
        && lhs.fragment == rhs.fragment;
    // clang-format on
}

}
