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

#include "coro/task.hpp"
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
    : writer(writer)
    , resp(base)
    , encode(encode)
{}

headers& response_writer::headers() noexcept { return resp->headers; }

coro::task<io::writer*> response_writer::send(status status_code, std::size_t content_length)
{
    // a second final response would be read as the response to the next request (by a proxy reusing the connection,
    // possibly another client's).
    if (final_sent) throw std::logic_error{"the response was already sent"};

    // 1xx and 204 responses have no content, and mustn't say how long it is; nor need a 304, whose length would have
    // to be the 200's (RFC 9110, section 8.6).
    const bool has_content =
        !status_is_information(status_code) && status_code != status::NO_CONTENT && status_code != status::NOT_MODIFIED;

    resp->status_code = status_code;
    if (has_content) resp->headers.set_content_length(content_length);
    else resp->headers.remove("Content-Length"sv);

    const auto body_length = has_content ? content_length : 0;

    // usually, the head fits in the connection's buffer: then there's no need for a coroutine (see coro::ready_task).
    auto encoding = encode(writer, *resp);
    if (encoding.finished_without_coroutine())
        return coro::ready_task<io::writer*>(take_encoding(std::move(encoding).result(), body_length));

    return finish_send(std::move(encoding), body_length);
}

coro::task<io::writer*> response_writer::finish_send(coro::task<response_encoder_result> encoding,
                                                     std::size_t                         body_length)
{
    co_return take_encoding(co_await std::move(encoding), body_length);
}

io::writer* response_writer::take_encoding(const response_encoder_result& res, std::size_t body_length)
{
    if (!res.has_value())
    {
        // e.g. a header that can't be written (one with a line break in it), which nothing was written for.
        resp->status_code = status::NONE;
        throw std::system_error{res.error().value(), res.error().category(), "failed to send response"};
    }

    if (!status_is_information(resp->status_code)) final_sent = true;

    body = io::limit_writer{resp->body, body_length};
    return &body;
}
}
