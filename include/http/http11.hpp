#pragma once

#include <system_error>
#include <type_traits>

#include "io/reader.hpp"
#include "io/writer.hpp"
#include "util/result.hpp"

namespace net::http
{

struct request;
struct server_response;
struct client_response;

}

namespace net::http::http11
{

using util::result;

std::error_condition request_encode(io::writer<char>& writer, const request& req) noexcept;
std::error_condition response_encode(io::writer<char>& writer, const server_response& resp) noexcept;

result<request, std::error_condition>         request_decode(io::reader<char>& reader) noexcept;
result<client_response, std::error_condition> response_decode(io::reader<char>& reader) noexcept;

}
