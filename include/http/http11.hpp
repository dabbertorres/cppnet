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

// check_request checks that a request can be encoded, as request_encode does first. It fails with:
// - std::errc::invalid_argument if a header field is malformed, or the body doesn't fit its framing (e.g. a
//   Content-Length without a body, or a body without one in HTTP/1.0, which can't send it chunked).
// - std::errc::not_supported for transfer codings other than chunked, or versions other than HTTP/1.0 and 1.1.
std::error_condition check_request(const client_request& req) noexcept;

// request_encode writes a request, including its body: with its Content-Length (which it must match), or chunked.
// POST, PUT and PATCH requests without bodies are sent with "Content-Length: 0".
task<request_encoder_result> request_encode(io::writer* writer, const client_request& req) noexcept;

task<response_encoder_result> response_encode(io::writer* writer, const server_response& resp) noexcept;

// max_fields is the most field lines a message's head may have (as hyper's default): headers are looked up by going
// through them, so a head of many tiny fields would otherwise take time quadratic in their number to decode.
constexpr std::size_t max_fields = 100;

// request_decode decodes a request's head, and fails with:
// - std::errc::invalid_argument if it's malformed (or ambiguous, e.g. two different Content-Lengths).
// - std::errc::not_supported if it uses something that isn't implemented (e.g. an unknown method, or gzip transfer
//   coding).
// - std::errc::value_too_large if the head is longer than max_header_bytes, or has more than max_fields field lines.
// - status_condition::closed if the connection ends first.
task<request_decoder_result>
request_decode(io::buffered_reader* reader,
               std::size_t          max_header_bytes = std::numeric_limits<std::size_t>::max()) noexcept;

// request_decode decodes a request's head into req, as the other does, reusing req's memory (e.g. for each request on
// a connection): its strings, fields, and body (if it's the reader the body needs). If it fails, req is left part-way
// decoded.
task<std::error_condition>
request_decode(io::buffered_reader* reader, std::size_t max_header_bytes, server_request& req) noexcept;

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
