#pragma once

#include <cstdint>
#include <iosfwd>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "tcp.hpp"

namespace net
{
    class reader;
    class writer;
}

namespace net::http
{

enum class method
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

constexpr std::string_view method_string(method m) noexcept;
constexpr method parse_method(std::string_view str) noexcept;

struct request
{
    request(method method, reader& body)
        : body{body}
    {}

    method method;
    std::string protocol;
    std::string host;
    std::string path;
    std::unordered_map<std::string, std::string> params;
    std::unordered_map<std::string, std::string> headers;

    reader& body;
};

struct response
{
    response(writer& body)
        : body{body}
    {}

    std::unordered_map<std::string, std::string> headers;

    void encode(writer& w) const;

    writer& body;
};

void parse_headers(reader& sock, request& req);

}
