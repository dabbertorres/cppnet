#pragma once

#include <cstddef>
#include <limits>
#include <memory>
#include <system_error>

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

task<result<io::writer*, std::error_condition>> request_encode(io::writer* writer, const client_request& req) noexcept;

task<result<io::writer*, std::error_condition>> response_encode(io::writer*            writer,
                                                                const server_response& resp) noexcept;

task<result<server_request, std::error_condition>>
request_decode(std::unique_ptr<io::buffered_reader> reader,
               std::size_t max_header_bytes = std::numeric_limits<std::size_t>::max()) noexcept;

task<result<client_response, std::error_condition>>
response_decode(std::unique_ptr<io::buffered_reader> reader,
                std::size_t max_header_bytes = std::numeric_limits<std::size_t>::max()) noexcept;

}
