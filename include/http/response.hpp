#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <string_view>
#include <system_error>
#include <utility>

#include "coro/task.hpp"
#include "http/headers.hpp"
#include "http/http.hpp"
#include "http/request.hpp"
#include "io/buffered_reader.hpp"
#include "io/limit_writer.hpp"
#include "io/reader.hpp"
#include "io/writer.hpp"

namespace net::http
{

enum class status : std::uint16_t
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

// reason_phrase is the status's reason phrase (RFC 9110, section 15), or empty for a status it doesn't know.
constexpr std::string_view reason_phrase(status s) noexcept
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
    case IM_USED: return "IM Used";

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
    case HTTP_VERSION_NOT_SUPPORTED: return "HTTP Version Not Supported";
    case VARIANT_ALSO_NEGOTIATES: return "Variant Also Negotiates";
    case INSUFFICIENT_STORAGE: return "Insufficient Storage";
    case LOOP_DETECTED: return "Loop Detected";
    case NOT_EXTENDED: return "Not Extended";
    case NETWORK_AUTHENTICATION_REQUIRED: return "Network Authentication Required";
    case NETWORK_CONNECT_TIMEOUT_ERROR: return "Network Connect Timeout Error";

    default: return "";
    }
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
    protocol_version            version{};
    status                      status_code = status::NONE;
    net::http::headers          headers{};
    std::unique_ptr<io::reader> body = nullptr;

    client_response() noexcept = default;

    client_response(const client_response&)            = delete;
    client_response& operator=(const client_response&) = delete;

    client_response(client_response&& other) noexcept
        : version{std::exchange(other.version, {})}
        , status_code{std::exchange(other.status_code, status::NONE)}
        , headers{std::exchange(other.headers, {})}
        , body{std::exchange(other.body, nullptr)}
    {}

    client_response& operator=(client_response&& other) noexcept
    {
        if (this != std::addressof(other))
        {
            version     = std::exchange(other.version, {});
            status_code = std::exchange(other.status_code, status::NONE);
            headers     = std::exchange(other.headers, {});
            body        = std::exchange(other.body, nullptr);
        }

        return *this;
    }

    ~client_response() noexcept = default;
};

// server_response represents an outgoing HTTP response from a server to a client.
struct server_response
{
    protocol_version   version{};
    status             status_code = status::NONE;
    net::http::headers headers{};
    io::writer*        body = nullptr;
};

using response_encoder_result = std::expected<io::writer*, std::error_condition>;
using response_decoder_result = std::expected<client_response, std::error_condition>;

using response_encoder = coro::task<response_encoder_result> (*)(io::writer*, const server_response&) noexcept;
using response_decoder = coro::task<response_decoder_result> (*)(std::unique_ptr<io::buffered_reader>,
                                                                 request_method,
                                                                 std::size_t) noexcept;

// response_writer presents an interface for building a server_response to request handlers.
struct response_writer
{
public:
    response_writer(io::writer* writer, server_response* base, response_encoder encoder);

    net::http::headers& headers() noexcept;

    // send sends the response's status and headers, with a Content-Length of content_length (except for statuses
    // that have no content: 1xx, 204 and 304), and returns the writer for its body. Writing more than content_length
    // fails with std::errc::value_too_large; writing less leaves the server to close the connection.
    coro::task<io::writer*> send(status status_code, std::size_t content_length);

    // body_remaining is how much of the body the handler has yet to write.
    [[nodiscard]] std::size_t body_remaining() const noexcept { return body.remaining(); }

    // status_code is the status that was sent, or status::NONE if the response hasn't been yet.
    [[nodiscard]] status status_code() const noexcept { return resp->status_code; }

private:
    io::writer*      writer;
    server_response* resp;
    response_encoder encode;
    io::limit_writer body;
};

}
