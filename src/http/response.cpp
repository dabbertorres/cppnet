#include "http/response.hpp"

#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>

#include "coro/task.hpp"
#include "http/chunked_writer.hpp"
#include "http/headers.hpp"
#include "io/buffered_writer.hpp"
#include "io/discard_writer.hpp"
#include "io/limit_writer.hpp"
#include "io/reader.hpp"
#include "io/writer.hpp"

namespace net::http
{

using namespace std::string_view_literals;

namespace
{

    // flush sends what's been written to writer at once, if it's buffered (as a server's connection's writer is: see
    // io::flush_before_read).
    coro::task<std::error_code> flush(io::writer* writer)
    {
        auto* buffered = dynamic_cast<io::buffered_writer*>(writer);
        if (buffered == nullptr) co_return std::error_code{};
        co_return (co_await buffered->flush()).err;
    }

}

std::string format_date(std::chrono::system_clock::time_point t)
{
    // NOTE: std::format's day and month names are the C locale's (the L option would use another), as HTTP's are.
    return std::format("{:%a, %d %b %Y %H:%M:%S} GMT", std::chrono::floor<std::chrono::seconds>(t));
}

const trailer_fields& client_response::trailers() const noexcept
{
    // NOTE: a body the caller put in place of the client's has none.
    static const trailer_fields none;

    const auto* source = dynamic_cast<const detail::trailer_source*>(body.get());
    return source != nullptr ? source->trailers() : none;
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

response_writer::response_writer(io::writer*            writer,
                                 server_response*       base,
                                 response_encoder       encode,
                                 const std::stop_token* close_after)
    : resp{
          base
}
    , backend{http11_backend{.writer = writer, .encode = encode, .close_after = close_after}}
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

trailer_fields& response_writer::trailers() noexcept { return resp->trailers; }

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

    // nothing can switch protocols (Upgrade is ignored): to an HTTP/1.1 client, a 101 would say the connection had
    // switched, and HTTP/2 has none (RFC 9113, section 8.6).
    if (status_code == status::SWITCHING_PROTOCOLS)
        throw std::system_error{std::make_error_code(std::errc::not_supported), "can't switch protocols"};

    const bool interim = status_is_information(status_code);

    // an HTTP/1.0 client doesn't know interim responses, and would take one for the final response (RFC 9110, section
    // 15.2: a server MUST NOT send it one).
    if (interim && std::holds_alternative<http11_backend>(backend) && resp->version.major == 1
        && resp->version.minor == 0)
    {
        static io::discard_writer dropped;
        return coro::ready_task<io::writer*>(&dropped);
    }

    // 1xx and 204 responses have no content, and mustn't say how long it is; nor need a 304, whose length would have
    // to be the 200's (RFC 9110, section 8.6).
    const bool has_content = !interim && status_code != status::NO_CONTENT && status_code != status::NOT_MODIFIED;
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
        const bool end = how == framing::length && body_length == 0 && !interim;

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

    // the server's closing, or the client waits for a 100 Continue it won't get before it sends the body (which then
    // may never come): the connection closes after this response, if it's the final one, which says so (replacing
    // e.g. keep-alive). An interim response says nothing of it: the 100 is still owed after one.
    const bool closing =
        (h1.close_after != nullptr && h1.close_after->stop_requested()) || continuation != continue_state::none;
    if (!interim && closing && !resp->headers.has_token("Connection"sv, "close"sv))
        resp->headers.set("Connection"sv, "close"sv);

    // usually, the head fits in the connection's buffer: then there's no need for a coroutine (see coro::ready_task).
    auto encoding = h1.encode(h1.writer, *resp);

    // sent at once, rather than with the final response, which is its point (e.g. Early Hints).
    if (interim) return send_interim(std::move(encoding));
    if (encoding.finished_without_coroutine())
        return coro::ready_task<io::writer*>(take_encoding(std::move(encoding).result(), how, body_length));

    return finish_send(std::move(encoding), how, body_length);
}

coro::task<io::writer*>
response_writer::finish_send(coro::task<response_encoder_result> encoding, framing how, std::size_t body_length)
{
    co_return take_encoding(co_await std::move(encoding), how, body_length);
}

coro::task<io::writer*> response_writer::send_interim(coro::task<response_encoder_result> encoding)
{
    auto* body = take_encoding(co_await std::move(encoding), framing::length, 0);

    // NOTE: the server's writer is buffered (see io::flush_before_read), and nothing else writes to it meanwhile: the
    //       handler is what writes, and a watch's read doesn't flush.
    if (auto err = co_await flush(std::get<http11_backend>(backend).writer))
        throw std::system_error{err.value(), err.category(), "failed to send response"};

    co_return body;
}

io::writer* response_writer::take_encoding(const response_encoder_result& res, framing how, std::size_t body_length)
{
    if (!res.has_value())
    {
        // e.g. a header that can't be written (one with a line break in it), which nothing was written for.
        resp->status_code = status::NONE;
        throw std::system_error{res.error().value(), res.error().category(), "failed to send response"};
    }

    const bool is_final = !status_is_information(resp->status_code);
    if (is_final) final_sent = true;

    // the handler's own 100 meets the expectation, and a final response sent before the body's asked for leaves it
    // unmet, as the client may wait for one regardless. Another interim response (e.g. Early Hints) does neither: the
    // body's first read still sends a 100, as Go's server does.
    if (resp->status_code == status::CONTINUE) continuation = continue_state::none;
    else if (is_final && continuation == continue_state::owed) continuation = continue_state::unmet;

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

io::start_callback response_writer::expect_continue() noexcept
{
    continuation = continue_state::owed;
    return {.call = &response_writer::body_starts, .context = this};
}

coro::task<std::error_code> response_writer::body_starts(void* context)
{
    auto& rw = *static_cast<response_writer*>(context);

    // the handler's reading the body regardless (after its final response): the client sends it when it gives up
    // waiting, so there's no more reason to close the connection after the response.
    if (rw.continuation == continue_state::unmet) rw.continuation = continue_state::none;
    if (rw.continuation != continue_state::owed) return coro::ready_task<std::error_code>();

    rw.continuation = continue_state::none;
    return rw.send_continue();
}

coro::task<std::error_code> response_writer::send_continue()
{
    // just the status: the response's headers are the handler's, for its own response.
    const server_response head{.version = resp->version, .status_code = status::CONTINUE};

    if (const auto* h2 = std::get_if<http2_backend>(&backend))
    {
        auto sent = co_await h2->ops->send_head(*h2->stream, head, false);
        co_return sent.has_value() ? std::error_code{} : sent.error();
    }

    const auto& h1      = std::get<http11_backend>(backend);
    auto        encoded = co_await h1.encode(h1.writer, head);
    if (!encoded.has_value()) co_return encoded.error();

    co_return co_await flush(h1.writer);
}

coro::task<io::result> response_writer::finish()
{
    if (const auto* h2 = std::get_if<http2_backend>(&backend)) return h2->ops->finish(*h2->stream);

    auto& h1 = std::get<http11_backend>(backend);
    if (!h1.chunked_open) return coro::ready_task<io::result>();

    // NOTE: only a chunked body has a last chunk to carry trailers: a body framed otherwise drops them.
    h1.chunked_open = false;
    return h1.chunked.finish(&resp->trailers);
}
}
