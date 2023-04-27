#pragma once

#include <cstddef>
#include <limits>
#include <system_error>
#include <type_traits>

#include "http/request.hpp"
#include "http/response.hpp"
#include "io/buffered_reader.hpp"
#include "io/writer.hpp"
#include "util/result.hpp"

namespace net::http::http11
{

using util::result;

util::result<io::writer*, std::error_condition> request_encode(io::writer* writer, const client_request& req) noexcept;

util::result<io::writer*, std::error_condition> response_encode(io::writer*            writer,
                                                                const server_response& resp) noexcept;

result<server_request, std::error_condition>
request_decode(io::buffered_reader& reader,
               std::size_t          max_header_bytes = std::numeric_limits<std::size_t>::max()) noexcept;

result<client_response, std::error_condition>
response_decode(io::buffered_reader& reader,
                std::size_t          max_header_bytes = std::numeric_limits<std::size_t>::max()) noexcept;

}
