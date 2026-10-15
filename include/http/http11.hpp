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

// request_decode decodes a request's head, and fails with:
// - std::errc::invalid_argument if it's malformed (or ambiguous, e.g. two different Content-Lengths).
// - std::errc::not_supported if it uses something that isn't implemented (e.g. an unknown method, or gzip transfer
//   coding).
// - std::errc::value_too_large if the head is longer than max_header_bytes.
// - status_condition::closed if the connection ends first.
task<request_decoder_result>
request_decode(io::buffered_reader* reader,
               std::size_t          max_header_bytes = std::numeric_limits<std::size_t>::max()) noexcept;

// response_decode decodes a response's head, failing as request_decode does. method is the request's: responses to
// HEAD requests have no body. The body owns reader.
task<response_decoder_result>
response_decode(std::unique_ptr<io::buffered_reader> reader,
                request_method                       method           = request_method::GET,
                std::size_t                          max_header_bytes = std::numeric_limits<std::size_t>::max()) noexcept;

// response_decode decodes a response's head, as above, but the body borrows reader, which must outlive it.
task<response_decoder_result> response_decode(io::buffered_reader* reader,
                                              request_method       method,
                                              std::size_t max_header_bytes = std::numeric_limits<std::size_t>::max()) noexcept;

// connection_reusable returns whether the connection can carry another request once the response's body has been read
// to its end: the response is HTTP/1.1, without "Connection: close", and its body doesn't end with the connection.
bool connection_reusable(const client_response& resp, request_method method) noexcept;

}
