#include "http/response.hpp"

#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>

#include "coro/task.hpp"
#include "http/chunked_writer.hpp"
#include "http/headers.hpp"
#include "io/limit_writer.hpp"
#include "io/writer.hpp"

namespace net::http
{

using namespace std::string_view_literals;

std::string format_date(std::chrono::system_clock::time_point t)
{
    // NOTE: std::format's day and month names are the C locale's (the L option would use another), as HTTP's are.
    return std::format("{:%a, %d %b %Y %H:%M:%S} GMT", std::chrono::floor<std::chrono::seconds>(t));
}

status parse_status(std::string_view str) noexcept
{
    using enum status;

    std::uint32_t v; // NOLINT(cppcoreguidelines-init-variables)
    const char*   begin = reinterpret_cast<const char*>(str.data());
    auto [_, err]       = std::from_chars(begin, begin + str.size(), v);

    if (static_cast<int>(err) != 0) return static_cast<status>(0);
    return static_cast<status>(v);
}

response_writer::response_writer(io::writer* writer, server_response* base, response_encoder encode)
    : resp{
          base
}
    , backend{http11_backend{.writer = writer, .encode = encode}}
{}

response_writer::response_writer(detail::http2_stream*           stream,
                                 const detail::http2_stream_ops* ops,
                                 server_response*                base) noexcept
    : resp{
          base
}
    , backend{http2_backend{.stream = stream, .ops = ops}}
{}

headers& response_writer::headers() noexcept { return resp->headers; }

coro::task<io::writer*> response_writer::send(status status_code, std::size_t content_length)
{
    return start(status_code, framing::length, content_length);
}

coro::task<io::writer*> response_writer::send(status status_code)
{
    // an HTTP/1.0 client doesn't know chunked bodies.
    const bool http10 = resp->version.major == 1 && resp->version.minor == 0;
    return start(status_code, http10 ? framing::close : framing::chunked, 0);
}

coro::task<io::writer*> response_writer::start(status status_code, framing how, std::size_t body_length)
{
    // a second final response would be read as the response to the next request (by a proxy reusing the connection,
    // possibly another client's).
    if (final_sent) throw std::logic_error{"the response was already sent"};

    // 1xx and 204 responses have no content, and mustn't say how long it is; nor need a 304, whose length would have
    // to be the 200's (RFC 9110, section 8.6).
    const bool has_content =
        !status_is_information(status_code) && status_code != status::NO_CONTENT && status_code != status::NOT_MODIFIED;
    if (!has_content)
    {
        how         = framing::length;
        body_length = 0;
    }

    resp->status_code = status_code;
    resp->headers.remove("Content-Length"sv);
    resp->headers.remove("Transfer-Encoding"sv);

    if (const auto* h2 = std::get_if<http2_backend>(&backend))
    {
        // HTTP/2 frames the body itself: with its length, if it's known; ended by finish() otherwise, or with the head
        // if there's none.
        if (how == framing::length && has_content) resp->headers.set_content_length(body_length);
        const bool end = how == framing::length && body_length == 0 && !status_is_information(status_code);

        auto sending = h2->ops->send_head(*h2->stream, *resp, end);
        if (sending.finished_without_coroutine())
            return coro::ready_task<io::writer*>(take_encoding(std::move(sending).result(), how, body_length));
        return finish_send(std::move(sending), how, body_length);
    }

    const auto& h1 = std::get<http11_backend>(backend);
    switch (how)
    {
    case framing::length:
        if (has_content) resp->headers.set_content_length(body_length);
        break;
    case framing::chunked: resp->headers.set("Transfer-Encoding"sv, "chunked"sv); break;
    case framing::close: resp->headers.set("Connection"sv, "close"sv); break;
    }

    // usually, the head fits in the connection's buffer: then there's no need for a coroutine (see coro::ready_task).
    auto encoding = h1.encode(h1.writer, *resp);
    if (encoding.finished_without_coroutine())
        return coro::ready_task<io::writer*>(take_encoding(std::move(encoding).result(), how, body_length));

    return finish_send(std::move(encoding), how, body_length);
}

coro::task<io::writer*>
response_writer::finish_send(coro::task<response_encoder_result> encoding, framing how, std::size_t body_length)
{
    co_return take_encoding(co_await std::move(encoding), how, body_length);
}

io::writer* response_writer::take_encoding(const response_encoder_result& res, framing how, std::size_t body_length)
{
    if (!res.has_value())
    {
        // e.g. a header that can't be written (one with a line break in it), which nothing was written for.
        resp->status_code = status::NONE;
        throw std::system_error{res.error().value(), res.error().category(), "failed to send response"};
    }

    if (!status_is_information(resp->status_code)) final_sent = true;

    // HTTP/2's stream gives the body's writer, which frames it; its length is still limited to what was sent.
    if (std::holds_alternative<http2_backend>(backend))
    {
        if (how != framing::length) return *res;
        body = io::limit_writer{*res, body_length};
        return &body;
    }

    auto& h1 = std::get<http11_backend>(backend);
    switch (how)
    {
    case framing::length: body = io::limit_writer{resp->body, body_length}; return &body;

    case framing::chunked:
        h1.chunked      = http11::chunked_writer{resp->body};
        h1.chunked_open = true;
        return &h1.chunked;

    case framing::close: return resp->body;
    }

    return resp->body;
}

coro::task<io::result> response_writer::finish()
{
    if (const auto* h2 = std::get_if<http2_backend>(&backend)) return h2->ops->finish(*h2->stream);

    auto& h1 = std::get<http11_backend>(backend);
    if (!h1.chunked_open) return coro::ready_task<io::result>();

    h1.chunked_open = false;
    return h1.chunked.finish();
}
}
