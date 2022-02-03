#include "http/http.hpp"

#include <charconv>

#include "encoding.hpp"
#include "socket.hpp"
#include "util.hpp"

namespace net::http
{

method parse_method(std::string_view str) noexcept
{
    using enum method;

    if (str == "CONNECT") return CONNECT;
    if (str == "DELETE") return DELETE;
    if (str == "GET") return GET;
    if (str == "HEAD") return HEAD;
    if (str == "OPTIONS") return OPTIONS;
    if (str == "PATCH") return PATCH;
    if (str == "POST") return POST;
    if (str == "PUT") return PUT;
    if (str == "TRACE") return TRACE;
    return NONE;
}

status parse_status(std::string_view str) noexcept
{
    using enum status;

    uint32_t v;
    auto     res = std::from_chars(str.begin(), str.end(), v);
    if (res.ptr != str.end()) return static_cast<status>(0);

    return static_cast<status>(v);
}

constexpr std::string_view method_string(method m) noexcept
{
    using enum method;

    switch (m)
    {
    case CONNECT:
        return "CONNECT";
    case DELETE:
        return "DELETE";
    case GET:
        return "GET";
    case HEAD:
        return "HEAD";
    case OPTIONS:
        return "OPTIONS";
    case PATCH:
        return "PATCH";
    case POST:
        return "POST";
    case PUT:
        return "PUT";
    case TRACE:
        return "TRACE";
    default:
        return "NONE";
    }
}

constexpr std::string_view status_string(status s) noexcept
{
    using enum status;

    switch (s)
    {
    case CONTINUE:
        return "Continue";
    case SWITCHING_PROTOCOLS:
        return "Switching Protocols";
    case PROCESSING:
        return "Processing";
    case OK:
        return "OK";
    case CREATED:
        return "Created";
    case ACCEPTED:
        return "Accepted";
    case NON_AUTHORITATIVE_INFORMATION:
        return "Non-Authoritative Information";
    case NO_CONTENT:
        return "No Content";
    case RESET_CONTENT:
        return "Reset Content";
    case PARTIAL_CONTENT:
        return "Partial Content";
    case MULTI_STATUS:
        return "Multi-Status";
    case ALREADY_REPORTED:
        return "Already Reported";
    case IM_USED:
        return "Im Used";
    case MULTIPLE_CHOICES:
        return "Multiple Choices";
    case MOVED_PERMANENTLY:
        return "Moved Permanently";
    case FOUND:
        return "Found";
    case SEE_OTHER:
        return "See Other";
    case NOT_MODIFIED:
        return "Not Modified";
    case USE_PROXY:
        return "Use Proxy";
    case TEMPORARY_REDIRECT:
        return "Temporary Redirect";
    case PERMANENT_REDIRECT:
        return "Permanent Redirect";
    case BAD_REQUEST:
        return "Bad Request";
    case UNAUTHORIZED:
        return "Unauthorized";
    case PAYMENT_REQUIRED:
        return "Payment Required";
    case FORBIDDEN:
        return "Forbidden";
    case NOT_FOUND:
        return "Not Found";
    case METHOD_NOT_ALLOWED:
        return "Method Not Allowed";
    case NOT_ACCEPTABLE:
        return "Not Acceptable";
    case PROXY_AUTHENTICATION_REQUIRED:
        return "Proxy Authentication Required";
    case REQUEST_TIMEOUT:
        return "Request Timeout";
    case CONFLICT:
        return "Conflict";
    case GONE:
        return "Gone";
    case LENGTH_REQUIRED:
        return "Length Required";
    case PRECONDITION_FAILED:
        return "Precondition Failed";
    case PAYLOAD_TOO_LARGE:
        return "Payload Too Large";
    case REQUEST_URI_TOO_LONG:
        return "Request-Uri Too Long";
    case UNSUPPORTED_MEDIA_TYPE:
        return "Unsupported Media Type";
    case REQUESTED_RANGE_NOT_SATISFIABLE:
        return "Requested Range Not Satisfiable";
    case EXPECTATION_FAILED:
        return "Expectation Failed";
    case IM_A_TEAPOT:
        return "I'm A Teapot";
    case MISDIRECTED_REQUEST:
        return "Misdirected Request";
    case UNPROCESSABLE_ENTITY:
        return "Unprocessable Entity";
    case LOCKED:
        return "Locked";
    case FAILED_DEPENDENCY:
        return "Failed Dependency";
    case UPGRADE_REQUIRED:
        return "Upgrade Required";
    case PRECONDITION_REQUIRED:
        return "Precondition Required";
    case TOO_MANY_REQUESTS:
        return "Too Many Requests";
    case REQUEST_HEADER_FIELDS_TOO_LARGE:
        return "Request Header Fields Too Large";
    case CONNECTION_CLOSED_WITHOUT_RESPONSE:
        return "Connection Closed Without Response";
    case UNAVAILABLE_FOR_LEGAL_REASONS:
        return "Unavailable For Legal Reasons";
    case CLIENT_CLOSED_REQUEST:
        return "Client Closed Request";
    case INTERNAL_SERVER_ERROR:
        return "Internal Server Error";
    case NOT_IMPLEMENTED:
        return "Not Implemented";
    case BAD_GATEWAY:
        return "Bad Gateway";
    case SERVICE_UNAVAILABLE:
        return "Service Unavailable";
    case GATEWAY_TIMEOUT:
        return "Gateway Timeout";
    case HTTP_VERSION_NOT_SUPPORTED:
        return "Http Version Not Supported";
    case VARIANT_ALSO_NEGOTIATES:
        return "Variant Also Negotiates";
    case INSUFFICIENT_STORAGE:
        return "Insufficient Storage";
    case LOOP_DETECTED:
        return "Loop Detected";
    case NOT_EXTENDED:
        return "Not Extended";
    case NETWORK_AUTHENTICATION_REQUIRED:
        return "Network Authentication Required";
    case NETWORK_CONNECT_TIMEOUT_ERROR:
        return "Network Connect Timeout Error";
    default:
        return "Unknown";
    }
}

}
