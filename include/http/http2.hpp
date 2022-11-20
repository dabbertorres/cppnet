#pragma once

#include <cstddef>
#include <string>

#include "http/request.hpp"
#include "http/response.hpp"
#include "io/buffered_reader.hpp"
#include "io/writer.hpp"
#include "util/result.hpp"

namespace net::http::http2
{

using util::result;

std::error_condition request_encode(io::writer& writer, const client_request& req) noexcept;
std::error_condition response_encode(const server_response& resp) noexcept;

result<server_request, std::error_condition>
request_decode(io::buffered_reader& reader,
               std::size_t          max_header_bytes = std::numeric_limits<std::size_t>::max()) noexcept;

result<server_response, std::error_condition>
response_decode(io::buffered_reader& reader,
                std::size_t          max_header_bytes = std::numeric_limits<std::size_t>::max()) noexcept;

}
