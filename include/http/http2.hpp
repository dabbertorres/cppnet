#pragma once

#include <cstddef>
#include <limits>
#include <memory>

#include "http/request.hpp"
#include "http/response.hpp"
#include "io/buffered_reader.hpp"
#include "io/writer.hpp"

namespace net::http::http2
{

request_encoder_result request_encode(io::writer* writer, const client_request& req) noexcept;

response_encoder_result response_encode(io::writer* writer, const server_response& resp) noexcept;

request_decoder_result request_decode(std::unique_ptr<io::buffered_reader>&& reader,
                                      std::size_t max_header_bytes = std::numeric_limits<std::size_t>::max()) noexcept;

response_decoder_result
response_decode(std::unique_ptr<io::buffered_reader>&& reader,
                std::size_t max_header_bytes = std::numeric_limits<std::size_t>::max()) noexcept;

}
