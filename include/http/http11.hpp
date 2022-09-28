#pragma once

#include <limits>
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

// TODO: split client/server stuff into separate files?

std::error_condition request_encode(io::writer<char>& writer, const request& req) noexcept;
std::error_condition response_encode(io::writer<char>& writer, const server_response& resp) noexcept;

std::error_condition
read_headers(io::reader<char>& reader, request& req, size_t max_read = std::numeric_limits<size_t>::max()) noexcept;

std::error_condition read_headers(io::reader<char>& reader,
                                  client_response&  resp,
                                  size_t            max_read = std::numeric_limits<size_t>::max()) noexcept;

result<request, std::error_condition>
request_decode(io::reader<char>& reader, size_t max_header_bytes = std::numeric_limits<size_t>::max()) noexcept;

result<client_response, std::error_condition>
response_decode(io::reader<char>& reader, size_t max_header_bytes = std::numeric_limits<size_t>::max()) noexcept;

}
