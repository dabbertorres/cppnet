#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>

#include "http/headers.hpp"
#include "http/http.hpp"
#include "io/limit_reader.hpp"
#include "io/writer.hpp"
#include "util/result.hpp"

namespace net::http
{

enum class status : std::uint32_t
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

status parse_status(std::string_view str) noexcept;

constexpr std::string_view status_text(status s) noexcept
{
    using enum status;
    switch (s)
    {
    case CONTINUE: return "100 Continue";
    case SWITCHING_PROTOCOLS: return "101 Switching Protocols";
    case PROCESSING: return "102 Processing";

    case OK: return "200 OK";
    case CREATED: return "201 Created";
    case ACCEPTED: return "202 Accepted";
    case NON_AUTHORITATIVE_INFORMATION: return "203 Non-Authoritative Information";
    case NO_CONTENT: return "204 No Content";
    case RESET_CONTENT: return "205 Reset Content";
    case PARTIAL_CONTENT: return "206 Partial Content";
    case MULTI_STATUS: return "207 Multi-Status";
    case ALREADY_REPORTED: return "208 Already Reported";
    case IM_USED: return "226 IM Used";

    case MULTIPLE_CHOICES: return "300 Multiple Choices";
    case MOVED_PERMANENTLY: return "301 Moved Permanently";
    case FOUND: return "302 Found";
    case SEE_OTHER: return "303 See Other";
    case NOT_MODIFIED: return "304 Not Modified";
    case USE_PROXY: return "305 Use Proxy";
    case TEMPORARY_REDIRECT: return "307 Temporary Redirect";
    case PERMANENT_REDIRECT: return "308 Permanent Redirect";

    case BAD_REQUEST: return "400 Bad Request";
    case UNAUTHORIZED: return "401 Unauthorized";
    case PAYMENT_REQUIRED: return "402 Payment Required";
    case FORBIDDEN: return "403 Forbidden";
    case NOT_FOUND: return "404 Not Found";
    case METHOD_NOT_ALLOWED: return "405 Method Not Allowed";
    case NOT_ACCEPTABLE: return "406 Not Acceptable";
    case PROXY_AUTHENTICATION_REQUIRED: return "407 Proxy Authentication Required";
    case REQUEST_TIMEOUT: return "408 Request Timeout";
    case CONFLICT: return "409 Conflict";
    case GONE: return "410 Gone";
    case LENGTH_REQUIRED: return "411 Length Required";
    case PRECONDITION_FAILED: return "412 Precondition Failed";
    case PAYLOAD_TOO_LARGE: return "413 Payload Too Large";
    case REQUEST_URI_TOO_LONG: return "414 Request-Uri Too Long";
    case UNSUPPORTED_MEDIA_TYPE: return "415 Unsupported Media Type";
    case REQUESTED_RANGE_NOT_SATISFIABLE: return "416 Requested Range Not Satisfiable";
    case EXPECTATION_FAILED: return "417 Expectation Failed";
    case IM_A_TEAPOT: return "418 I'm A Teapot";
    case MISDIRECTED_REQUEST: return "421 Misdirected Request";
    case UNPROCESSABLE_ENTITY: return "422 Unprocessable Entity";
    case LOCKED: return "423 Locked";
    case FAILED_DEPENDENCY: return "424 Failed Dependency";
    case UPGRADE_REQUIRED: return "426 Upgrade Required";
    case PRECONDITION_REQUIRED: return "428 Precondition Required";
    case TOO_MANY_REQUESTS: return "429 Too Many Requests";
    case REQUEST_HEADER_FIELDS_TOO_LARGE: return "431 Request Header Fields Too Large";
    case CONNECTION_CLOSED_WITHOUT_RESPONSE: return "444 Connection Closed Without Response";
    case UNAVAILABLE_FOR_LEGAL_REASONS: return "451 Unavailable For Legal Reasons";
    case CLIENT_CLOSED_REQUEST: return "499 Client Closed Request";

    case INTERNAL_SERVER_ERROR: return "500 Internal Server Error";
    case NOT_IMPLEMENTED: return "501 Not Implemented";
    case BAD_GATEWAY: return "502 Bad Gateway";
    case SERVICE_UNAVAILABLE: return "503 Service Unavailable";
    case GATEWAY_TIMEOUT: return "504 Gateway Timeout";
    case HTTP_VERSION_NOT_SUPPORTED: return "505 Http Version Not Supported";
    case VARIANT_ALSO_NEGOTIATES: return "506 Variant Also Negotiates";
    case INSUFFICIENT_STORAGE: return "507 Insufficient Storage";
    case LOOP_DETECTED: return "508 Loop Detected";
    case NOT_EXTENDED: return "510 Not Extended";
    case NETWORK_AUTHENTICATION_REQUIRED: return "511 Network Authentication Required";
    case NETWORK_CONNECT_TIMEOUT_ERROR:
        return "599 Network Connect Timeout Error";

    [[unlikely]] default:
        return "XXX Unknown";
    }
}

constexpr std::string_view status_string(status s) noexcept
{
    return status_text(s).substr(4); // chop off the status code
}

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
constexpr bool status_is_server_error(status s) noexcept { return status::INTERNAL_SERVER_ERROR <= s; }

// client_response represents an incoming HTTP response from a server to a client.
struct client_response
{
    protocol_version version{};
    status           status_code = status::NONE;
    headers          headers;
    io::limit_reader body;
};

// server_response represents an outgoing HTTP response from a server to a client.
struct server_response
{
    protocol_version version{};
    status           status_code = status::NONE;
    headers          headers;
    io::writer*      body = nullptr;
};

using response_encoder = util::result<io::writer*, std::error_condition> (*)(io::writer* writer,
                                                                             const server_response&);

// response_writer presents an interface for building a server_response to request handlers.
struct response_writer
{
public:
    response_writer(io::writer* writer, server_response* base, response_encoder encoder);

    headers&    headers() noexcept;
    io::writer& send(status status_code, std::size_t content_length);

private:
    io::writer*      writer;
    server_response* resp;
    response_encoder encode;
};

}
