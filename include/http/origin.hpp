#pragma once

#include <expected>
#include <string>
#include <string_view>
#include <system_error>

// https origins, as HTTP/2's ORIGIN frame lists them (RFC 8336): the server's http2_origins, and the client's Origin
// Sets. Not for applications.
namespace net::http::detail
{

// https_origin is an https origin as ORIGIN lists it: its ASCII serialization (RFC 6454, section 6.2), normalized
// (lowercase, the default port, 443, left out, an address written as ip_addr writes it, an IPv6 one in brackets); and
// its host, as a certificate is checked for it (an IPv6 address without its brackets).
struct https_origin
{
    std::string serialized;
    std::string host;
};

// parse_origin reads an https origin, "https://host" or "https://host:port", normalized: the scheme in any case; the
// host a DNS name of letters, digits and hyphens (labels of 1 to 63, neither starting nor ending with a hyphen, 253 in
// all; no wildcard, nor a final dot), an IPv4 address, or an IPv6 one in brackets; the port 1 to 65535; and nothing
// else (no user info, no path, not even "/"). It fails with std::errc::invalid_argument for anything else.
std::expected<https_origin, std::error_code> parse_origin(std::string_view text);

// origin_text is the https origin of a URL's host (an IPv6 address without brackets, as url has it) and port (none, if
// it's empty), as text for parse_origin(): not normalized, nor checked.
std::string origin_text(std::string_view host, std::string_view port);

}
