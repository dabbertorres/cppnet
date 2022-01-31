#include "http/http.hpp"

#include <ostream>

#include "encoding.hpp"
#include "socket.hpp"
#include "util.hpp"

namespace net::http
{

constexpr std::string_view method_string(method m) noexcept
{
    switch (m)
    {
    case method::CONNECT:
        return "CONNECT";
    case method::DELETE:
        return "DELETE";
    case method::GET:
        return "GET";
    case method::HEAD:
        return "HEAD";
    case method::OPTIONS:
        return "OPTIONS";
    case method::PATCH:
        return "PATCH";
    case method::POST:
        return "POST";
    case method::PUT:
        return "PUT";
    case method::TRACE:
        return "TRACE";
    default:
        return "NONE";
    }
}

constexpr method parse_method(std::string_view str) noexcept
{
    if (str == "CONNECT") return method::CONNECT;
    if (str == "DELETE") return method::DELETE;
    if (str == "GET") return method::GET;
    if (str == "HEAD") return method::HEAD;
    if (str == "OPTIONS") return method::OPTIONS;
    if (str == "PATCH") return method::PATCH;
    if (str == "POST") return method::POST;
    if (str == "PUT") return method::PUT;
    if (str == "TRACE") return method::TRACE;
    return method::NONE;
}

std::string read_line(reader& r) {}

void response::encode(writer& w) const
{
    for (auto& kv : headers)
    {
        body << kv.first << ": " << kv.second << '\n';
    }
}

void parse_headers(reader& sock, request& req) {}

}
