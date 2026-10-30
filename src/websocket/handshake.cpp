#include "websocket/handshake.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <expected>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "encoding/base64.hpp"
#include "http/headers.hpp"
#include "http/http.hpp"
#include "http/request.hpp"
#include "http/request_target.hpp"
#include "http/response.hpp"
#include "url.hpp"
#include "util/random.hpp"
#include "util/sha1.hpp"
#include "util/string_util.hpp"
#include "websocket/error.hpp"
#include "websocket/websocket.hpp"

namespace
{

using namespace std::string_view_literals;

using net::websocket::errc;

// key_size is how many bytes a Sec-WebSocket-Key is, before base64 (RFC 6455, section 4.1), and key_chars how many
// characters, after.
constexpr std::size_t key_size  = 16;
constexpr std::size_t key_chars = net::encoding::base64::encoded_size(key_size);

std::unexpected<std::error_code> failure(errc err) noexcept { return std::unexpected(make_error_code(err)); }

// only_value is key's value, if it has exactly one line.
std::optional<std::string_view> only_value(const net::http::headers& headers, std::string_view key)
{
    const auto values = headers.get_all(key);
    if (!values || std::ranges::distance(*values) != 1) return std::nullopt;
    return std::string_view{*values->begin()};
}

// upgrades is whether a message's head says it's switching to WebSockets.
bool upgrades(const net::http::headers& headers)
{
    return headers.has_token("Upgrade"sv, "websocket"sv) && headers.has_token("Connection"sv, "upgrade"sv);
}

bool has_body(const net::http::headers& headers)
{
    return headers.get("Transfer-Encoding"sv).has_value() || headers.get_content_length().value_or(0) != 0;
}

bool valid_key(std::string_view key)
{
    if (key.size() != key_chars) return false;

    std::array<std::byte, net::encoding::base64::decoded_size(key_chars)> decoded{};
    const auto size = net::encoding::base64::encoding{}.decode_to(key, decoded);
    return size && *size == key_size;
}

// authority is a host and port, as an origin, a request or a pattern has them (an IPv6 address without its brackets).
struct authority
{
    std::string_view host;
    std::string_view port; // empty if there's none
};

// split_authority splits "host[:port]", whose host may be an IPv6 address in brackets, without checking either.
std::optional<authority> split_authority(std::string_view text)
{
    if (text.starts_with('['))
    {
        const auto end = text.find(']');
        if (end == std::string_view::npos) return std::nullopt;

        const auto rest = text.substr(end + 1);
        if (!rest.empty() && !rest.starts_with(':')) return std::nullopt;
        return authority{.host = text.substr(1, end - 1), .port = rest.empty() ? ""sv : rest.substr(1)};
    }

    const auto colon = text.rfind(':');
    if (colon == std::string_view::npos) return authority{.host = text, .port = {}};
    return authority{.host = text.substr(0, colon), .port = text.substr(colon + 1)};
}

// origin is an Origin's parts: its scheme, and its host and port.
struct origin
{
    std::string_view scheme;
    authority        where;
};

// parse_origin reads an origin's serialization (RFC 6454, section 6.2): "scheme://host[:port]", nothing after; the
// host as a Host field's (a name, or an IP address, IPv6 in brackets), the port a number.
std::optional<origin> parse_origin(std::string_view text)
{
    const auto separator = text.find("://"sv);
    if (separator == std::string_view::npos || separator == 0) return std::nullopt;

    const auto scheme       = text.substr(0, separator);
    const auto valid_scheme = std::ranges::all_of(
        scheme,
        [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '+' || c == '-' || c == '.'; });
    if (!valid_scheme || std::isalpha(static_cast<unsigned char>(scheme.front())) == 0) return std::nullopt;

    // parse_host checks the host and port (as a Host field's): split_authority finds the same parts, as views.
    const auto rest = text.substr(separator + 3);
    if (!net::http::parse_host(rest)) return std::nullopt;
    const auto where = split_authority(rest);
    if (!where || where->host.empty()) return std::nullopt;
    return origin{.scheme = scheme, .where = *where};
}

// default_port is a scheme's default port, as an origin leaves it out; empty for one this doesn't know.
std::string_view default_port(std::string_view scheme)
{
    if (net::util::equal_ignore_case(scheme, "http"sv) || net::util::equal_ignore_case(scheme, "ws"sv)) return "80"sv;
    if (net::util::equal_ignore_case(scheme, "https"sv) || net::util::equal_ignore_case(scheme, "wss"sv))
        return "443"sv;
    return ""sv;
}

// same_port is whether two ports (either empty, for none) are the same, one left out being the scheme's default.
bool same_port(std::string_view lhs, std::string_view rhs, std::string_view scheme)
{
    return (lhs.empty() ? default_port(scheme) : lhs) == (rhs.empty() ? default_port(scheme) : rhs);
}

// request_authority is the host and port the request is for: its target's, if it's in absolute form, otherwise its
// Host field's. (The decoder checked both.)
authority request_authority(const net::http::server_request& req)
{
    if (!req.target.host.empty()) return {.host = req.target.host, .port = req.target.port};

    const auto host = split_authority(req.headers.get("Host"sv).value_or(""sv));
    return host ? *host : authority{};
}

// host_matches is whether host matches pattern, label by label: each the same, ignoring case, or * in the pattern.
bool host_matches(std::string_view pattern, std::string_view host)
{
    auto patterns = net::util::split_string(pattern, '.');
    auto labels   = net::util::split_string(host, '.');

    auto p = patterns.begin();
    auto l = labels.begin();
    for (; p != patterns.end() && l != labels.end(); ++p, ++l)
    {
        if (*p != "*"sv && !net::util::equal_ignore_case(*p, *l)) return false;
    }
    return p == patterns.end() && l == labels.end();
}

// pattern_matches is whether an origin matches an accept_options::origins pattern.
bool pattern_matches(std::string_view pattern, const origin& from)
{
    const auto allowed = split_authority(pattern);
    if (!allowed || !host_matches(allowed->host, from.where.host)) return false;

    if (allowed->port == "*"sv) return true;
    return same_port(allowed->port, from.where.port, from.scheme);
}

}

namespace net::websocket::detail
{

std::string accept_key(std::string_view key)
{
    const auto digest = util::sha1{}.update(key).update(guid).finish();
    return encoding::base64::encoding{}.encode(digest);
}

std::expected<std::string_view, std::error_code> check_request(const http::server_request& req)
{
    // over HTTP/2, an extended CONNECT (RFC 8441, section 5), which the server presents as GET with its :protocol: it
    // has no Upgrade, Connection nor key (the stream's the proof that the server took it).
    if (req.version.major == 2)
    {
        if (req.method != http::request_method::GET
            || !util::equal_ignore_case(std::string_view{req.protocol}, "websocket"sv) || has_body(req.headers))
            return failure(errc::not_websocket);
        if (only_value(req.headers, "Sec-WebSocket-Version"sv) != "13"sv) return failure(errc::bad_version);
        return ""sv;
    }

    if (req.version.major != 1 || req.version.minor < 1) return failure(errc::not_websocket);
    if (req.method != http::request_method::GET || has_body(req.headers) || !upgrades(req.headers))
        return failure(errc::not_websocket);

    if (only_value(req.headers, "Sec-WebSocket-Version"sv) != "13"sv) return failure(errc::bad_version);

    const auto key = only_value(req.headers, "Sec-WebSocket-Key"sv);
    if (!key || !valid_key(*key)) return failure(errc::bad_key);
    return *key;
}

std::string_view select_subprotocol(std::span<const std::string> server, const http::headers& request)
{
    if (server.empty()) return {};

    const auto offered = request.get_list("Sec-WebSocket-Protocol"sv);
    for (const auto& protocol : server)
    {
        if (std::ranges::contains(offered, std::string_view{protocol})) return protocol;
    }
    return {};
}

std::error_code check_origin(const http::server_request& req, const accept_options& options)
{
    const auto values = req.headers.get_all("Origin"sv);
    if (!values || options.insecure_any_origin) return {};

    const auto refused = make_error_code(errc::origin_refused);
    const auto value   = only_value(req.headers, "Origin"sv);
    if (!value) return refused;

    // NOTE: "null" (an opaque origin) isn't an origin with a host, so parse_origin() refuses it.
    const auto from = parse_origin(*value);
    if (!from) return refused;

    const auto own = request_authority(req);
    if (util::equal_ignore_case(own.host, from->where.host) && same_port(own.port, from->where.port, from->scheme))
        return {};

    const auto matches = [&](const std::string& pattern) { return pattern_matches(pattern, *from); };
    return std::ranges::any_of(options.origins, matches) ? std::error_code{} : refused;
}

client_handshake make_request(const url& target, const connect_options& options)
{
    std::array<std::byte, key_size> random{};
    util::random_bytes(random);

    client_handshake out{.request = {}, .key = encoding::base64::encoding{}.encode(random)};
    auto&            request = out.request;
    request.method           = http::request_method::GET;
    request.version          = {.major = 1, .minor = 1};
    request.uri              = target;
    request.headers          = options.headers;

    request.headers.remove("Sec-WebSocket-Extensions"sv);
    request.headers.set("Upgrade"sv, "websocket"sv);
    request.headers.set("Connection"sv, "Upgrade"sv);
    request.headers.set("Sec-WebSocket-Key"sv, out.key);
    request.headers.set("Sec-WebSocket-Version"sv, "13"sv);

    if (options.subprotocols.empty())
    {
        request.headers.remove("Sec-WebSocket-Protocol"sv);
    }
    else
    {
        std::string offered;
        for (const auto& protocol : options.subprotocols)
        {
            if (!offered.empty()) offered.append(", "sv);
            offered.append(protocol);
        }
        request.headers.set("Sec-WebSocket-Protocol"sv, std::move(offered));
    }

    if (!options.origin.empty()) request.headers.set("Origin"sv, options.origin);
    return out;
}

std::expected<std::string_view, std::error_code>
check_response(const http::client_response& response, std::string_view key, std::span<const std::string> offered)
{
    if (response.status_code != http::status::SWITCHING_PROTOCOLS) return failure(errc::bad_status);
    if (!upgrades(response.headers)) return failure(errc::not_websocket);
    if (only_value(response.headers, "Sec-WebSocket-Accept"sv) != accept_key(key)) return failure(errc::bad_accept);
    if (!response.headers.get_list("Sec-WebSocket-Extensions"sv).empty()) return failure(errc::bad_extension);

    if (!response.headers.get("Sec-WebSocket-Protocol"sv)) return ""sv;

    const auto chosen = only_value(response.headers, "Sec-WebSocket-Protocol"sv);
    if (!chosen || !std::ranges::contains(offered, *chosen)) return failure(errc::bad_subprotocol);
    return *chosen;
}

}
