#pragma once

#include <cstddef>
#include <expected>
#include <limits>
#include <memory>

#include "coro/task.hpp"
#include "http/request.hpp"
#include "http/response.hpp"
#include "io/buffered_reader.hpp"
#include "io/writer.hpp"
#include "util/result.hpp"

namespace net::http::http11
{

using coro::task;
using util::result;

task<request_encoder_result> request_encode(io::writer* writer, const client_request& req) noexcept;

task<response_encoder_result> response_encode(io::writer* writer, const server_response& resp) noexcept;

task<request_decoder_result>
request_decode(std::unique_ptr<io::buffered_reader> reader,
               std::size_t max_header_bytes = std::numeric_limits<std::size_t>::max()) noexcept;

task<response_decoder_result>
response_decode(std::unique_ptr<io::buffered_reader> reader,
                std::size_t max_header_bytes = std::numeric_limits<std::size_t>::max()) noexcept;

}
