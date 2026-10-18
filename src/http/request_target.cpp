#include "http/request_target.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <expected>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <sys/socket.h>

#include <arpa/inet.h>
#include <netinet/in.h>

#include "http/request.hpp"
#include "url.hpp"
#include "util/string_util.hpp"

namespace
{

using namespace std::string_view_literals;

using net::http::request_target;

// Character classes from RFC 3986, section 2.

constexpr bool is_alpha(char c) noexcept { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
constexpr bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }
constexpr bool is_hex_digit(char c) noexcept { return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

constexpr bool is_unreserved(char c) noexcept
{
    return is_alpha(c) || is_digit(c) || c == '-' || c == '.' || c == '_' || c == '~';
}

constexpr bool is_sub_delim(char c) noexcept { return "!$&'()*+,;="sv.contains(c); }

constexpr bool is_pchar(char c) noexcept { return is_unreserved(c) || is_sub_delim(c) || c == ':' || c == '@'; }

constexpr bool is_path_char(char c) noexcept { return is_pchar(c) || c == '/'; }
constexpr bool is_query_char(char c) noexcept { return is_pchar(c) || c == '/' || c == '?'; }
constexpr bool is_reg_name_char(char c) noexcept { return is_unreserved(c) || is_sub_delim(c); }

// is_valid checks that every character of str is allowed, or part of a valid percent-encoding.
template<typename Allowed>
bool is_valid(std::string_view str, Allowed allowed) noexcept
{
    for (std::size_t i = 0; i < str.size(); ++i)
    {
        if (str[i] == '%')
        {
            if (str.size() - i < 3 || !is_hex_digit(str[i + 1]) || !is_hex_digit(str[i + 2])) return false;
            i += 2;
        }
        else if (!allowed(str[i]))
        {
            return false;
        }
    }

    return true;
}

// remove_dot_segments removes "." and ".." segments from an absolute path, as RFC 3986 section 5.2.4 does, but also
// treating their percent-encodings (e.g. "%2e%2E") as dots, as the WHATWG URL standard does.
std::string remove_dot_segments(std::string_view path)
{
    std::vector<std::string_view> out;

    std::vector<std::string_view> segments;
    for (auto segment : net::util::split_string(path.substr(1), '/')) segments.push_back(segment);

    for (std::size_t i = 0; i < segments.size(); ++i)
    {
        const bool last    = i + 1 == segments.size();
        const auto decoded = net::url::decode(segments[i]);

        if (decoded == ".." && !out.empty()) out.pop_back();

        if (decoded == "." || decoded == "..")
        {
            // keep the trailing slash: "/a/b/.." is "/a/".
            if (last) out.emplace_back();
            continue;
        }

        out.push_back(segments[i]);
    }

    std::string result = "/";
    for (std::size_t i = 0; i < out.size(); ++i)
    {
        if (i != 0) result += '/';
        result += out[i];
    }

    return result;
}

bool parse_port(std::string_view port, request_target& out)
{
    if (port.empty() || port.size() > 5 || !std::ranges::all_of(port, is_digit)) return false;

    unsigned value = 0;
    std::from_chars(port.data(), port.data() + port.size(), value);
    if (value > 65'535) return false;

    out.port = port;
    return true;
}

// parse_authority parses "host[:port]" (where host may be an IPv6 literal, in brackets).
bool parse_authority(std::string_view authority, bool require_port, request_target& out)
{
    std::string_view port;
    bool             has_port = false;

    if (authority.starts_with('['))
    {
        auto end = authority.find(']');
        if (end == std::string_view::npos) return false;

        std::string literal{authority.substr(1, end - 1)};
        in6_addr    addr{};
        if (::inet_pton(AF_INET6, literal.c_str(), &addr) != 1) return false;
        out.host = std::move(literal);

        auto rest = authority.substr(end + 1);
        if (!rest.empty())
        {
            if (rest.front() != ':') return false;
            has_port = true;
            port     = rest.substr(1);
        }
    }
    else
    {
        auto colon = authority.rfind(':');
        auto host  = authority.substr(0, colon);
        if (colon != std::string_view::npos)
        {
            has_port = true;
            port     = authority.substr(colon + 1);
        }

        if (host.empty() || !is_valid(host, is_reg_name_char)) return false;
        out.host = host;
    }

    if (has_port) return parse_port(port, out);
    return !require_port;
}

// parse_path_and_query parses what follows the authority (if any): an absolute path, and an optional query.
bool parse_path_and_query(std::string_view rest, request_target& out)
{
    auto query_start = rest.find('?');
    auto path        = rest.substr(0, query_start);

    if (query_start != std::string_view::npos)
    {
        auto query = rest.substr(query_start + 1);
        if (!is_valid(query, is_query_char)) return false;
        out.query = query;
    }

    if (!is_valid(path, is_path_char)) return false;

    // a dot segment has a dot, perhaps encoded: without one, the path is kept as it is (without allocating for it).
    const bool may_have_dots = path.contains('.') || path.contains("%2e"sv) || path.contains("%2E"sv);

    if (path.empty()) out.path.assign("/"sv);
    else if (!may_have_dots) out.path.assign(path);
    else out.path = remove_dot_segments(path);

    return true;
}

bool parse_absolute_form(std::string_view target, request_target& out)
{
    auto scheme_end = target.find("://"sv);
    if (scheme_end == std::string_view::npos) return false;

    // Only http(s) URIs make sense to an HTTP server.
    auto scheme = target.substr(0, scheme_end);
    if (!net::util::equal_ignore_case(scheme, "http"sv) && !net::util::equal_ignore_case(scheme, "https"sv))
        return false;

    auto rest          = target.substr(scheme_end + 3);
    auto authority_end = std::min(rest.find('/'), rest.find('?'));
    auto authority     = rest.substr(0, authority_end);

    // userinfo isn't allowed in http(s) URIs (RFC 9110, section 4.2.4).
    if (authority.contains('@')) return false;
    if (!parse_authority(authority, false, out)) return false;

    out.kind = request_target::form::absolute;
    return parse_path_and_query(authority_end == std::string_view::npos ? ""sv : rest.substr(authority_end), out);
}

}

namespace net::http
{

url::query_values request_target::query_values() const { return url::parse_query(query); }

std::expected<request_target, std::error_code> parse_request_target(request_method   method,
                                                                    std::string_view target) noexcept
{
    request_target out;
    if (auto err = parse_request_target(method, target, out); err) return std::unexpected(err);
    return out;
}

std::error_code parse_request_target(request_method method, std::string_view target, request_target& out) noexcept
{
    const auto invalid = std::make_error_code(std::errc::invalid_argument);

    try
    {
        // NOTE: emptied, rather than replaced, to keep their memory.
        out.kind = request_target::form::origin;
        out.path.assign("/"sv);
        out.query.clear();
        out.host.clear();
        out.port.clear();

        bool ok = false;

        if (method == request_method::CONNECT)
        {
            ok       = parse_authority(target, true, out);
            out.kind = request_target::form::authority;
        }
        else if (target == "*"sv)
        {
            ok       = method == request_method::OPTIONS;
            out.kind = request_target::form::asterisk;
        }
        else if (target.starts_with('/'))
        {
            ok = parse_path_and_query(target, out);
        }
        else
        {
            ok = parse_absolute_form(target, out);
        }

        if (!ok) return invalid;
        return {};
    }
    catch (...)
    {
        // only allocation can fail.
        return std::make_error_code(std::errc::not_enough_memory);
    }
}

std::expected<request_target, std::error_code> parse_host(std::string_view value) noexcept
{
    try
    {
        request_target out;
        out.kind = request_target::form::authority;

        if (!parse_authority(value, false, out))
            return std::unexpected(std::make_error_code(std::errc::invalid_argument));

        return out;
    }
    catch (...)
    {
        // only allocation can fail.
        return std::unexpected(std::make_error_code(std::errc::not_enough_memory));
    }
}

}
