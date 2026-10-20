#include "url.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

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

constexpr bool is_hex_digit(char c) noexcept { return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

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
constexpr bool is_scheme_char(char c) noexcept
{
    return is_alpha(c) || is_digit(c) || c == '+' || c == '-' || c == '.';
}

// Characters that may appear unencoded in each part (RFC 3986).
constexpr bool is_userinfo_char(char c) noexcept { return is_unreserved(c) || is_sub_delim(c); }
constexpr bool is_host_char(char c) noexcept { return is_unreserved(c) || is_sub_delim(c); }
constexpr bool is_path_char(char c) noexcept
{
    return is_unreserved(c) || is_sub_delim(c) || c == ':' || c == '@' || c == '/';
}
constexpr bool is_query_char(char c) noexcept { return is_path_char(c) || c == '?'; }
constexpr bool is_fragment_char(char c) noexcept { return is_query_char(c); }

// The WHATWG URL standard's forbidden domain code points (in ASCII): a host containing these, once decoded, could be
// mistaken for another part of a URL, or end a header line.
constexpr bool is_forbidden_host_char(char c) noexcept
{
    auto byte = static_cast<unsigned char>(c);
    return byte <= 0x20 || byte == 0x7F || "#%/:<>?@[\\]^|"sv.contains(c);
}

template<typename Allowed>
void percent_encode(std::string& out, char c, Allowed allowed)
{
    static constexpr auto hex = "0123456789ABCDEF"sv;

    if (allowed(c))
    {
        out += c;
        return;
    }

    auto byte = static_cast<unsigned char>(c);
    out += '%';
    out += hex[byte >> 4];
    out += hex[byte & 0x0F];
}

template<typename Allowed>
void percent_encode(std::string& out, std::string_view str, Allowed allowed)
{
    for (char c : str) percent_encode(out, c, allowed);
}

// append_encoded appends str to out as it is, but for what can't appear in it unencoded: a character that isn't
// allowed, or a '%' that doesn't start an encoding, is percent-encoded. Encodings are kept as written (e.g. "%2f").
template<typename Allowed>
void append_encoded(std::string& out, std::string_view str, Allowed allowed)
{
    for (std::size_t i = 0; i < str.size(); ++i)
    {
        if (str[i] == '%' && str.size() - i > 2 && is_hex_digit(str[i + 1]) && is_hex_digit(str[i + 2]))
        {
            out += str[i];
            out += str[i + 1];
            out += str[i + 2];
            i += 2;
        }
        else
        {
            percent_encode(out, str[i], allowed);
        }
    }
}

bool has_authority(const url& u) noexcept { return !u.host.empty() || !u.userinfo.empty() || !u.port.empty(); }

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
        if (rest.front() != ':')
            return url_parse_failure{.failed_at = url_parse_state::port, .index = offset + end + 1};

        return parse_port(rest.substr(1), offset + end + 2, out);
    }

    auto colon = authority.find(':');

    out.host = url::decode(authority.substr(0, colon));
    if (std::ranges::any_of(out.host, is_forbidden_host_char))
        return url_parse_failure{.failed_at = url_parse_state::host, .index = offset};

    if (colon == std::string_view::npos) return std::nullopt;
    return parse_port(authority.substr(colon + 1), offset + colon + 1, out);
}

// reference is a URI reference split into its parts, with whether it has the parts that may be empty.
struct reference
{
    url  parts;
    bool authority = false;
    bool query     = false;
};

enum class reading : std::uint8_t
{
    url,    // as url::parse() reads it: without a scheme or "//", and not starting with '/', it starts with its host.
    strict, // as RFC 3986's URI-reference.
};

std::expected<reference, url_parse_failure> split(std::string_view input, reading how)
{
    reference   ref;
    url&        out = ref.parts;
    std::size_t pos = 0;

    // scheme: ALPHA *( ALPHA / DIGIT / "+" / "-" / "." ) ":"
    if (auto colon = input.find(':'); colon != std::string_view::npos && colon > 0 && is_alpha(input.front())
                                      && std::ranges::all_of(input.substr(0, colon), is_scheme_char))
    {
        out.scheme = input.substr(0, colon);
        pos        = colon + 1;
    }

    auto rest = input.substr(pos);

    ref.authority = rest.starts_with("//"sv);
    const bool starts_with_host =
        how == reading::url && !ref.authority && out.scheme.empty() && !rest.empty() && !"/?#"sv.contains(rest.front());

    if (ref.authority || starts_with_host)
    {
        auto authority_start = ref.authority ? pos + 2 : pos;
        auto authority_end   = input.find_first_of("/?#"sv, authority_start);
        if (authority_end == std::string_view::npos) authority_end = input.size();

        if (auto err =
                parse_authority(input.substr(authority_start, authority_end - authority_start), authority_start, out))
            return std::unexpected(*err);

        ref.authority = true;
        pos           = authority_end;
    }

    auto path_end = input.find_first_of("?#"sv, pos);
    if (path_end == std::string_view::npos) path_end = input.size();
    const auto path       = input.substr(pos, path_end - pos);
    const auto path_start = pos;
    pos                   = path_end;

    // a relative path's first segment can't have a ':', which would be read as a scheme (RFC 3986, section 4.2).
    if (how == reading::strict && out.scheme.empty() && !ref.authority && path.substr(0, path.find('/')).contains(':'))
        return std::unexpected(url_parse_failure{.failed_at = url_parse_state::path, .index = path_start});

    append_encoded(out.path, path, is_path_char);

    if (pos < input.size() && input[pos] == '?')
    {
        auto query_end = input.find('#', pos);
        if (query_end == std::string_view::npos) query_end = input.size();
        append_encoded(out.query, input.substr(pos + 1, query_end - pos - 1), is_query_char);
        ref.query = true;
        pos       = query_end;
    }

    if (pos < input.size() && input[pos] == '#') append_encoded(out.fragment, input.substr(pos + 1), is_fragment_char);

    return ref;
}

// remove_dot_segments removes the "." and ".." segments of a path, as RFC 3986 section 5.2.4 does.
std::string remove_dot_segments(std::string_view in)
{
    std::string out;
    out.reserve(in.size());

    // pop removes the output's last segment, and the '/' before it.
    const auto pop = [&]
    {
        const auto slash = out.rfind('/');
        out.resize(slash == std::string::npos ? 0 : slash);
    };

    while (!in.empty())
    {
        if (in.starts_with("../"sv))
        {
            in.remove_prefix(3);
        }
        else if (in.starts_with("./"sv) || in.starts_with("/./"sv))
        {
            in.remove_prefix(2);
        }
        else if (in == "/."sv)
        {
            in = "/"sv;
        }
        else if (in.starts_with("/../"sv))
        {
            in.remove_prefix(3);
            pop();
        }
        else if (in == "/.."sv)
        {
            in = "/"sv;
            pop();
        }
        else if (in == "."sv || in == ".."sv)
        {
            in = {};
        }
        else
        {
            // the first segment, with the '/' before it.
            const auto end = std::min(in.find('/', 1), in.size());
            out.append(in.substr(0, end));
            in.remove_prefix(end);
        }
    }

    return out;
}

// merge merges a relative path with the base's (RFC 3986, section 5.2.3).
std::string merge(const url& base, std::string_view path)
{
    if (has_authority(base) && base.path.empty()) return "/" + std::string{path};

    const auto slash = base.path.rfind('/');
    if (slash == std::string::npos) return std::string{path};
    return base.path.substr(0, slash + 1) + std::string{path};
}

std::expected<url, std::error_code> resolve_reference(const url& base, std::string_view text)
{
    const auto invalid = std::make_error_code(std::errc::invalid_argument);

    // RFC 3986 needs an absolute base (section 5.1), but one with an authority suffices (e.g. url::parse("host/a")),
    // as the client sends requests to it.
    if (base.scheme.empty() && !has_authority(base)) return std::unexpected(invalid);

    auto split_ref = split(text, reading::strict);
    if (!split_ref.has_value()) return std::unexpected(invalid);
    auto& [ref, ref_authority, ref_query] = *split_ref;

    // section 5.2.2, with "strict" parsing (a reference with the base's scheme is still absolute).
    url target;
    if (!ref.scheme.empty())
    {
        target      = std::move(ref);
        target.path = remove_dot_segments(target.path);
        return target;
    }

    target.scheme = base.scheme;

    if (ref_authority)
    {
        // as for the base, the url must have a scheme or an authority: it can't hold an empty authority without a
        // scheme (it would build as a relative reference).
        if (target.scheme.empty() && !has_authority(ref)) return std::unexpected(invalid);

        target.userinfo = std::move(ref.userinfo);
        target.host     = std::move(ref.host);
        target.port     = std::move(ref.port);
        target.path     = remove_dot_segments(ref.path);
        target.query    = std::move(ref.query);
    }
    else
    {
        target.userinfo = base.userinfo;
        target.host     = base.host;
        target.port     = base.port;

        if (ref.path.empty())
        {
            target.path = base.path;
            if (ref_query) target.query = std::move(ref.query);
            else target.query = base.query;
        }
        else
        {
            target.path  = remove_dot_segments(ref.path.starts_with('/') ? ref.path : merge(base, ref.path));
            target.query = std::move(ref.query);
        }
    }

    target.fragment = std::move(ref.fragment);
    return target;
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

        auto  eq             = pair.find('=');
        auto& values_for_key = values[decode_form(pair.substr(0, eq))];
        if (eq != std::string_view::npos) values_for_key.push_back(decode_form(pair.substr(eq + 1)));
    }

    return values;
}

std::expected<url, std::error_code> url::resolve(const url& base, std::string_view reference) noexcept
{
    try
    {
        return resolve_reference(base, reference);
    }
    catch (...)
    {
        // only allocation can fail.
        return std::unexpected(std::make_error_code(std::errc::not_enough_memory));
    }
}

url::parse_result url::parse(const char* s) noexcept { return parse(std::string_view(s)); }

url::parse_result url::parse(const std::string& s) noexcept { return parse(std::string_view{s}); }

url::parse_result url::parse(std::string_view s) noexcept
{
    try
    {
        auto ref = split(s, reading::url);
        if (!ref.has_value()) return std::unexpected(ref.error());
        return std::move(ref->parts);
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
    if (has_authority(*this) || path.starts_with("//"))
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

    append_encoded(out, path, is_path_char);

    if (!query.empty())
    {
        out += '?';
        append_encoded(out, query, is_query_char);
    }

    if (!fragment.empty())
    {
        out += '#';
        append_encoded(out, fragment, is_fragment_char);
    }

    return out;
}

std::string url::origin_form() const
{
    std::string out;

    out.reserve(path.size() + query.size() + 2);

    if (!path.starts_with('/')) out += '/';
    append_encoded(out, path, is_path_char);

    if (!query.empty())
    {
        out += '?';
        append_encoded(out, query, is_query_char);
    }

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
