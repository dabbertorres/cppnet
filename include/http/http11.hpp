#pragma once

#include <iosfwd>
#include <streambuf>
#include <system_error>
#include <type_traits>

#include "reader.hpp"
#include "result.hpp"
#include "writer.hpp"

namespace net::http
{

struct request;
struct server_response;
struct client_response;

}

namespace net::http::http11
{

using util::result;

std::error_condition request_encode(net::writer<std::byte>& writer, const request& req) noexcept;
std::error_condition response_encode(net::writer<std::byte>& writer, const server_response& resp) noexcept;

result<request, std::error_condition>         request_decode(net::reader<std::byte>& reader) noexcept;
result<client_response, std::error_condition> response_decode(net::reader<std::byte>& reader) noexcept;

class body_streambuf : public std::streambuf
{
public:
    body_streambuf(const std::streambuf& src)
        : std::streambuf{src}
    {}

private:
};

}
