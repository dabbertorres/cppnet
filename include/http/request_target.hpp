#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <system_error>

#include "url.hpp"

namespace net::http
{

enum class request_method : std::uint8_t;

// request_target is the target of a request, as sent in its request line (RFC 9112, section 3.2).
struct request_target
{
    enum class form : std::uint8_t
    {
        origin,    // "/path?query": the usual form.
        absolute,  // "http://host/path?query": sent to proxies, but servers must accept it too.
        authority, // "host:port": only for CONNECT.
        asterisk,  // "*": only for OPTIONS.
    };

    form kind = form::origin;

    // The path, still percent-encoded, so that encoded characters (e.g. "%2F") can be told apart from the delimiters
    // they encode. Dot segments ("." and "..", encoded or not) are removed, so it can't go above "/". "/" for the
    // authority and asterisk forms.
    std::string path = "/";

    // The query, without the '?', and still percent-encoded.
    std::string query;

    // From the absolute and authority forms only.
    std::string host;
    std::string port;

    bool operator==(const request_target&) const = default;

    // query_values decodes the query, as application/x-www-form-urlencoded (so '+' is a space).
    [[nodiscard]] url::query_values query_values() const;
};

// parse_request_target parses and validates a request target. It fails with std::errc::invalid_argument if the target
// is malformed, or not allowed for the method.
std::expected<request_target, std::error_condition> parse_request_target(request_method   method,
                                                                         std::string_view target) noexcept;

}
