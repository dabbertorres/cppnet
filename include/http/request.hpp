#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "coro/task.hpp"
#include "http/headers.hpp"
#include "http/http.hpp"
#include "http/request_target.hpp"
#include "io/buffered_reader.hpp"
#include "io/reader.hpp"
#include "io/string_reader.hpp"
#include "io/writer.hpp"
#include "url.hpp"
#include "util/small_vector.hpp"
#include "util/string_util.hpp"
#include "util/type_map.hpp"

namespace net::http
{

enum class request_method : std::uint8_t
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
    case TRACE: return "TRACE";
    default: return "NONE";
    }
}

// client_request represents an outgoing HTTP request from a client to a server.
struct client_request
{
    request_method     method = request_method::GET;
    protocol_version   version{.major = 1, .minor = 1};
    url                uri{};
    net::http::headers headers{};

    // The body, which is read until it ends (or until Content-Length bytes have been read, if that's set, which it
    // must then match). Without a Content-Length, it's sent chunked, which needs HTTP/1.1. Sending the request reads
    // it, so it can only be sent once.
    std::unique_ptr<io::reader> body = nullptr;

    // set_body sets the body to content, and the Content-Length to match.
    void set_body(std::string content)
    {
        headers.set_content_length(content.size());
        body = std::make_unique<io::owned_string_reader>(std::move(content));
    }
};

// server_request represents an incoming HTTP request from a client to a server.
struct server_request
{
    request_method     method = request_method::NONE;
    protocol_version   version{};
    request_target     target{};
    net::http::headers headers{};

    // The host the request is for: from the target, if it has one, otherwise the Host header.
    std::string host{};

    // The pattern of the route that matched the request (e.g. "/users/:id"), which the router sets; empty if none did.
    // It views the router's copy, so is only valid while the router is (as it is while the router handles it).
    std::string_view route{};

    // The route's parameters (name, then decoded value), which the router sets: see path_value. The values view the
    // request's path, or path_value_storage if they needed decoding, so are only valid while the request is, and
    // isn't moved (as it isn't while it's being handled).
    util::small_vector<std::pair<std::string_view, std::string_view>, 4> path_values{};

    // path_value_storage holds the path_values that needed decoding, for the router: it's reserved to the length of
    // the rest of the path from the first of them, which they can't be longer than together, so they never move.
    std::string path_value_storage{};

    // Values for handling the request, which middleware passes to handlers: e.g. instrument::trace.
    util::type_map extensions{};

    std::unique_ptr<io::reader> body = nullptr;

    // path_value is the value of the route's parameter name: e.g. "42" for "id", for the route "/users/:id" and the
    // path "/users/42". It's empty if the route has no such parameter (a parameter never matches an empty segment).
    [[nodiscard]] std::string_view path_value(std::string_view name) const noexcept
    {
        const auto* iter = std::ranges::find(path_values, name, &std::pair<std::string_view, std::string_view>::first);
        return iter != path_values.end() ? iter->second : std::string_view{};
    }

    // add_path_value adds the parameter name, for encoded, which it decodes if need be. encoded must be part of
    // target.path, after any added before it, and name must outlive the request (e.g. being the router's).
    void add_path_value(std::string_view name, std::string_view encoded);

    // clear empties the request, to decode another into it, keeping its memory (and body, which decoding replaces).
    void clear() noexcept
    {
        method  = request_method::NONE;
        version = {};
        target.path.clear();
        target.query.clear();
        target.host.clear();
        target.port.clear();
        headers.clear();
        host.clear();
        route = {};
        path_values.clear();
        path_value_storage.clear();
        extensions.clear();
    }
};

using request_decoder_result = std::expected<server_request, std::error_code>;
using request_encoder_result = std::expected<io::writer*, std::error_code>;

// request_decoders borrow the connection's reader, which must outlive the decoded request's body. The reader is kept
// for the connection, rather than per request, so that anything buffered past the end of one request is still there
// for the next.
using request_decoder = coro::task<request_decoder_result> (*)(io::buffered_reader*, std::size_t) noexcept;
using request_encoder = coro::task<request_encoder_result> (*)(io::writer*, const client_request&) noexcept;

}
