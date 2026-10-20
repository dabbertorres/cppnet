#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <vector>

namespace net
{

enum class url_parse_state : std::uint8_t
{
    scheme,
    authority,
    userinfo,
    host,
    port,
    path,
    query,
    fragment,
    done,
};

constexpr std::string_view url_parse_state_to_string(url_parse_state s) noexcept
{
    using enum url_parse_state;

    switch (s)
    {
    case scheme: return "scheme";
    case authority: return "authority";
    case userinfo: return "userinfo";
    case host: return "host";
    case port: return "port";
    case path: return "path";
    case query: return "query";
    case fragment: return "fragment";
    case done: return "done";
    }
}

struct url_parse_failure
{
    url_parse_state failed_at;
    std::size_t     index;
};

class url
{
public:
    struct user_info
    {
        std::string username;
        std::string password;

        [[nodiscard]] bool empty() const;
    };

    using query_values = std::unordered_map<std::string, std::vector<std::string>>;

    static constexpr auto reserved_characters = "!#$%&'()*+,/:;=?@[]";

    // encode percent-encodes the characters in reserved, and any that aren't printable ASCII.
    static std::string encode(std::string_view str, std::string_view reserved = reserved_characters) noexcept;
    static std::string decode(std::string_view str) noexcept;
    // decode_append decodes str (as decode does) onto the end of out.
    static void decode_append(std::string& out, std::string_view str) noexcept;

    // parse_query decodes a query (without its '?') as application/x-www-form-urlencoded (so '+' is a space), e.g.
    // url::parse_query(u.query).
    static query_values parse_query(std::string_view query);

    using parse_result = std::expected<url, url_parse_failure>;

    // parse splits a URL as RFC 3986 does (with "//" before the authority), except that a URL with neither a scheme
    // nor "//", and not starting with '/', starts with its host (e.g. "example.com/path"). The path, query and fragment
    // are kept as written, percent-encodings and all (e.g. "%2F" isn't "/", and "%2f" stays "%2f"), except that what
    // can't appear in them unencoded (e.g. a space, a control, a non-ASCII byte, or a '%' that doesn't start an
    // encoding) is percent-encoded.
    static parse_result parse(const char* /*s*/) noexcept;
    static parse_result parse(const std::string& /*s*/) noexcept;
    static parse_result parse(std::string_view /*s*/) noexcept;

    // resolve resolves reference against base, as RFC 3986 section 5.2 does (e.g. "../page" against
    // "http://host/a/b" is "http://host/page"), removing the dot segments ("." and "..", not their encodings) from
    // the path it makes. It reads reference strictly, as a URI reference: "next/page" is a relative path, not a host
    // and a path, as parse() would read it. Its path, query and fragment are kept as parse() keeps them. It fails with
    // std::errc::invalid_argument if reference isn't a valid reference (e.g. its host or port is invalid, or its first
    // segment has a ':' without being a scheme), or base (or what it would make, e.g. from "//" against a base without
    // a scheme) has neither a scheme nor an authority.
    static std::expected<url, std::error_code> resolve(const url& base, std::string_view reference) noexcept;

    // build returns the URL: the user info and host percent-encoded as needed, and the path, query and fragment as they
    // are, but for what can't appear in them unencoded (as parse() encodes it).
    [[nodiscard]] std::string build() const noexcept;

    // origin_form returns the path and query, as sent in an HTTP request line (e.g. "/path?query"): as they are, but
    // for what can't appear in them unencoded (as build()), so nothing in them can end the line. It always starts with
    // '/'.
    [[nodiscard]] std::string origin_form() const;

    // The scheme and port, and the user info and host, decoded (an IPv6 host is without its brackets).
    std::string scheme;
    user_info   userinfo;
    std::string host;
    std::string port;

    // The path, query (without its '?') and fragment (without its '#'), percent-encoded: parse() keeps them as they
    // were written. An empty query or fragment is none, so "/path?" is built as "/path". Setting them, encode what's
    // meant as data rather than a delimiter (e.g. a query value's '&', with url::encode()).
    std::string path;
    std::string query;
    std::string fragment;
};

bool operator==(const url::user_info& lhs, const url::user_info& rhs) noexcept;
bool operator==(const url& lhs, const url& rhs) noexcept;

}
