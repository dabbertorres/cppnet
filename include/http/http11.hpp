#pragma once

#include <cstddef>
#include <limits>
#include <memory>
#include <system_error>

#include "coro/task.hpp"
#include "http/headers.hpp"
#include "http/request.hpp"
#include "http/response.hpp"
#include "io/buffered_reader.hpp"
#include "io/reader.hpp"
#include "io/writer.hpp"
#include "util/function_ref.hpp"

namespace net::http::http11
{

using coro::task;

// check_request checks that a request can be encoded, as request_encode does first. It fails with:
// - std::errc::invalid_argument if a header field is malformed, or the body doesn't fit its framing (e.g. a
//   Content-Length without a body, or a body without one in HTTP/1.0, which can't send it chunked).
// - std::errc::not_supported for transfer codings other than chunked, or versions other than HTTP/1.0 and 1.1.
std::error_code check_request(const client_request& req) noexcept;

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
task<std::error_code>
request_decode(io::buffered_reader* reader, std::size_t max_header_bytes, server_request& req) noexcept;

// call_at_body_end sets what's called once the body of a request that request_decode() decoded has been read to its
// end: by the body's read that reaches it, or now, if it's there already (e.g. it has none). For a server to watch the
// connection from then, e.g.
void call_at_body_end(const server_request& req, io::end_callback callback) noexcept;

// keep_body_trailers has the body of a request that request_decode() decoded keep its trailer section's fields in
// req.trailers (see chunked_reader::keep_trailers()), before call_at_body_end()'s callback is called: only a chunked
// body has any. req mustn't move while its body's read (as the server's, reused for each request on a connection,
// doesn't); without it, they're checked and dropped.
void keep_body_trailers(server_request& req) noexcept;

// call_at_body_start sets what's awaited before the first read of the body of a request that request_decode() decoded
// (see io::start_callback): e.g. for a server to send 100 Continue then. A body that's empty is never read, so there's
// nothing to call.
void call_at_body_start(const server_request& req, io::start_callback callback) noexcept;

// informational_ref is what response_decode() passes each interim (1xx) response to: its status and fields, which are
// only valid during the call.
using informational_ref = util::function_ref<void(status, const headers&)>;

// response_decode decodes a response's head, failing as request_decode does. method is the request's: responses to
// HEAD requests have no body. The body owns reader.
//
// Interim (1xx) responses before the final one are passed to on_informational, if it's set (otherwise skipped), as
// they're decoded: but 101, which is final (the connection is then the body). All of the response's heads, interim and
// final, count against max_header_bytes together. An exception on_informational throws comes out of the task, as it is
// (the reader's then part way through the response).
task<response_decoder_result> response_decode(std::unique_ptr<io::buffered_reader> reader,
                                              request_method                       method = request_method::GET,
                                              std::size_t max_header_bytes = std::numeric_limits<std::size_t>::max(),
                                              informational_ref on_informational = {}) noexcept;

// response_decode decodes a response's head, as above, but the body borrows reader, which must outlive it.
task<response_decoder_result> response_decode(io::buffered_reader* reader,
                                              request_method       method,
                                              std::size_t max_header_bytes = std::numeric_limits<std::size_t>::max(),
                                              informational_ref on_informational = {}) noexcept;

// connection_reusable returns whether the connection can carry another request once the response's body has been read
// to its end: the response is HTTP/1.1, without "Connection: close", and its body doesn't end with the connection.
bool connection_reusable(const client_response& resp, request_method method) noexcept;

}
