#pragma once

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <istream>
#include <iterator>
#include <memory>
#include <ostream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "reader.hpp"
#include "string_util.hpp"
#include "url.hpp"
#include "writer.hpp"

namespace net::http
{

enum class request_method
{
    CONNECT,
    DELETE,
    GET,
    HEAD,
    OPTIONS,
    PATCH,
    POST,
    PUT,
    TRACE,
    NONE,
};

enum class status : uint32_t
{
    NONE = 0,

    // 1×× Informational
    CONTINUE            = 100,
    SWITCHING_PROTOCOLS = 101,
    PROCESSING          = 102,
    // 2×× Success
    OK                            = 200,
    CREATED                       = 201,
    ACCEPTED                      = 202,
    NON_AUTHORITATIVE_INFORMATION = 203,
    NO_CONTENT                    = 204,
    RESET_CONTENT                 = 205,
    PARTIAL_CONTENT               = 206,
    MULTI_STATUS                  = 207,
    ALREADY_REPORTED              = 208,
    IM_USED                       = 226,
    // 3×× Redirection
    MULTIPLE_CHOICES   = 300,
    MOVED_PERMANENTLY  = 301,
    FOUND              = 302,
    SEE_OTHER          = 303,
    NOT_MODIFIED       = 304,
    USE_PROXY          = 305,
    TEMPORARY_REDIRECT = 307,
    PERMANENT_REDIRECT = 308,
    // 4×× Client Error
    BAD_REQUEST                        = 400,
    UNAUTHORIZED                       = 401,
    PAYMENT_REQUIRED                   = 402,
    FORBIDDEN                          = 403,
    NOT_FOUND                          = 404,
    METHOD_NOT_ALLOWED                 = 405,
    NOT_ACCEPTABLE                     = 406,
    PROXY_AUTHENTICATION_REQUIRED      = 407,
    REQUEST_TIMEOUT                    = 408,
    CONFLICT                           = 409,
    GONE                               = 410,
    LENGTH_REQUIRED                    = 411,
    PRECONDITION_FAILED                = 412,
    PAYLOAD_TOO_LARGE                  = 413,
    REQUEST_URI_TOO_LONG               = 414,
    UNSUPPORTED_MEDIA_TYPE             = 415,
    REQUESTED_RANGE_NOT_SATISFIABLE    = 416,
    EXPECTATION_FAILED                 = 417,
    IM_A_TEAPOT                        = 418,
    MISDIRECTED_REQUEST                = 421,
    UNPROCESSABLE_ENTITY               = 422,
    LOCKED                             = 423,
    FAILED_DEPENDENCY                  = 424,
    UPGRADE_REQUIRED                   = 426,
    PRECONDITION_REQUIRED              = 428,
    TOO_MANY_REQUESTS                  = 429,
    REQUEST_HEADER_FIELDS_TOO_LARGE    = 431,
    CONNECTION_CLOSED_WITHOUT_RESPONSE = 444,
    UNAVAILABLE_FOR_LEGAL_REASONS      = 451,
    CLIENT_CLOSED_REQUEST              = 499,
    // 5×× Server Error
    INTERNAL_SERVER_ERROR           = 500,
    NOT_IMPLEMENTED                 = 501,
    BAD_GATEWAY                     = 502,
    SERVICE_UNAVAILABLE             = 503,
    GATEWAY_TIMEOUT                 = 504,
    HTTP_VERSION_NOT_SUPPORTED      = 505,
    VARIANT_ALSO_NEGOTIATES         = 506,
    INSUFFICIENT_STORAGE            = 507,
    LOOP_DETECTED                   = 508,
    NOT_EXTENDED                    = 510,
    NETWORK_AUTHENTICATION_REQUIRED = 511,
    NETWORK_CONNECT_TIMEOUT_ERROR   = 599,
};

using headers_map = std::unordered_map<std::string, std::vector<std::string>>;

struct protocol_version
{
    uint32_t major;
    uint32_t minor;
};

struct request
{
    request_method   method;
    protocol_version version;
    url              uri;
    headers_map      headers;

    net::reader<std::byte>& body;
};

struct server_response
{
    protocol_version version;
    status           status_code;
    headers_map      headers;

    net::writer<std::byte>& body;
};

struct client_response
{
    protocol_version version;
    status           status_code;
    headers_map      headers;

    net::reader<std::byte>& body;
};

constexpr bool status_is_information(status s) noexcept { return status::CONTINUE <= s && s < status::OK; }

constexpr bool status_is_success(status s) noexcept { return status::OK <= s && s < status::MULTIPLE_CHOICES; }

constexpr bool status_is_redirection(status s) noexcept
{
    return status::MULTIPLE_CHOICES <= s && s < status::BAD_REQUEST;
}

constexpr bool status_is_client_error(status s) noexcept
{
    return status::BAD_REQUEST <= s && s < status::INTERNAL_SERVER_ERROR;
}

constexpr bool status_is_server_error(status s) noexcept
{
    return status::INTERNAL_SERVER_ERROR <= s && s < static_cast<status>(600);
}

constexpr request_method parse_method(std::string_view str) noexcept
{
    using enum request_method;
    using namespace std::literals::string_view_literals;

    if (util::equal_ignore_case(str, "CONNECT"sv)) return CONNECT;
    if (util::equal_ignore_case(str, "DELETE"sv)) return DELETE;
    if (util::equal_ignore_case(str, "GET"sv)) return GET;
    if (util::equal_ignore_case(str, "HEAD"sv)) return HEAD;
    if (util::equal_ignore_case(str, "OPTIONS"sv)) return OPTIONS;
    if (util::equal_ignore_case(str, "PATCH"sv)) return PATCH;
    if (util::equal_ignore_case(str, "POST"sv)) return POST;
    if (util::equal_ignore_case(str, "PUT"sv)) return PUT;
    if (util::equal_ignore_case(str, "TRACE"sv)) return TRACE;
    return NONE;
}

inline status parse_status(std::string_view str) noexcept
{
    using enum status;

    uint32_t v;
    auto     res = std::from_chars(str.begin(), str.end(), v);
    if (res.ptr != str.end()) return static_cast<status>(0);

    return static_cast<status>(v);
}

constexpr std::string_view method_string(request_method m) noexcept
{
    using enum request_method;

    switch (m)
    {
    case CONNECT: return "CONNECT";
    case DELETE: return "DELETE";
    case GET: return "GET";
    case HEAD: return "HEAD";
    case OPTIONS: return "OPTIONS";
    case PATCH: return "PATCH";
    case POST: return "POST";
    case PUT: return "PUT";
    case TRACE: return "TRACE";
    default: return "NONE";
    }
}

constexpr std::string_view status_string(status s) noexcept
{
    using enum status;

    switch (s)
    {
    case CONTINUE: return "Continue";
    case SWITCHING_PROTOCOLS: return "Switching Protocols";
    case PROCESSING: return "Processing";
    case OK: return "OK";
    case CREATED: return "Created";
    case ACCEPTED: return "Accepted";
    case NON_AUTHORITATIVE_INFORMATION: return "Non-Authoritative Information";
    case NO_CONTENT: return "No Content";
    case RESET_CONTENT: return "Reset Content";
    case PARTIAL_CONTENT: return "Partial Content";
    case MULTI_STATUS: return "Multi-Status";
    case ALREADY_REPORTED: return "Already Reported";
    case IM_USED: return "Im Used";
    case MULTIPLE_CHOICES: return "Multiple Choices";
    case MOVED_PERMANENTLY: return "Moved Permanently";
    case FOUND: return "Found";
    case SEE_OTHER: return "See Other";
    case NOT_MODIFIED: return "Not Modified";
    case USE_PROXY: return "Use Proxy";
    case TEMPORARY_REDIRECT: return "Temporary Redirect";
    case PERMANENT_REDIRECT: return "Permanent Redirect";
    case BAD_REQUEST: return "Bad Request";
    case UNAUTHORIZED: return "Unauthorized";
    case PAYMENT_REQUIRED: return "Payment Required";
    case FORBIDDEN: return "Forbidden";
    case NOT_FOUND: return "Not Found";
    case METHOD_NOT_ALLOWED: return "Method Not Allowed";
    case NOT_ACCEPTABLE: return "Not Acceptable";
    case PROXY_AUTHENTICATION_REQUIRED: return "Proxy Authentication Required";
    case REQUEST_TIMEOUT: return "Request Timeout";
    case CONFLICT: return "Conflict";
    case GONE: return "Gone";
    case LENGTH_REQUIRED: return "Length Required";
    case PRECONDITION_FAILED: return "Precondition Failed";
    case PAYLOAD_TOO_LARGE: return "Payload Too Large";
    case REQUEST_URI_TOO_LONG: return "Request-Uri Too Long";
    case UNSUPPORTED_MEDIA_TYPE: return "Unsupported Media Type";
    case REQUESTED_RANGE_NOT_SATISFIABLE: return "Requested Range Not Satisfiable";
    case EXPECTATION_FAILED: return "Expectation Failed";
    case IM_A_TEAPOT: return "I'm A Teapot";
    case MISDIRECTED_REQUEST: return "Misdirected Request";
    case UNPROCESSABLE_ENTITY: return "Unprocessable Entity";
    case LOCKED: return "Locked";
    case FAILED_DEPENDENCY: return "Failed Dependency";
    case UPGRADE_REQUIRED: return "Upgrade Required";
    case PRECONDITION_REQUIRED: return "Precondition Required";
    case TOO_MANY_REQUESTS: return "Too Many Requests";
    case REQUEST_HEADER_FIELDS_TOO_LARGE: return "Request Header Fields Too Large";
    case CONNECTION_CLOSED_WITHOUT_RESPONSE: return "Connection Closed Without Response";
    case UNAVAILABLE_FOR_LEGAL_REASONS: return "Unavailable For Legal Reasons";
    case CLIENT_CLOSED_REQUEST: return "Client Closed Request";
    case INTERNAL_SERVER_ERROR: return "Internal Server Error";
    case NOT_IMPLEMENTED: return "Not Implemented";
    case BAD_GATEWAY: return "Bad Gateway";
    case SERVICE_UNAVAILABLE: return "Service Unavailable";
    case GATEWAY_TIMEOUT: return "Gateway Timeout";
    case HTTP_VERSION_NOT_SUPPORTED: return "Http Version Not Supported";
    case VARIANT_ALSO_NEGOTIATES: return "Variant Also Negotiates";
    case INSUFFICIENT_STORAGE: return "Insufficient Storage";
    case LOOP_DETECTED: return "Loop Detected";
    case NOT_EXTENDED: return "Not Extended";
    case NETWORK_AUTHENTICATION_REQUIRED: return "Network Authentication Required";
    case NETWORK_CONNECT_TIMEOUT_ERROR: return "Network Connect Timeout Error";
    default: return "Unknown";
    }
}

}
