#pragma once

#include <string_view>

#include "http/headers.hpp"
#include "http/http.hpp"
#include "io/buffered_reader.hpp"
#include "io/limit_reader.hpp"
#include "util/string_util.hpp"

#include "url.hpp"

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

    [[unlikely]] return NONE;
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
    case TRACE:
        return "TRACE";

    [[unlikely]] default:
        return "NONE";
    }
}

// client_request represents an outgoing HTTP request from a client to a server.
struct client_request
{
    request_method     method = request_method::NONE;
    protocol_version   version{};
    url                uri{};
    net::http::headers headers{};

    io::reader* body = nullptr;
};

// server_request represents an incoming HTTP request from a client to a server.
struct server_request
{
    request_method     method = request_method::NONE;
    protocol_version   version{};
    url                uri{};
    net::http::headers headers{};

    io::limit_reader body;
};

using request_decoder_result = util::result<server_request, std::error_condition>;

using request_decoder = request_decoder_result (*)(io::buffered_reader&, std::size_t);

}
