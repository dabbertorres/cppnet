#include "http/response.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <system_error>

#include "coro/task.hpp"
#include "http/headers.hpp"
#include "io/limit_writer.hpp"
#include "io/writer.hpp"

namespace net::http
{

using namespace std::string_view_literals;

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
    // 1xx and 204 responses have no content, and mustn't say how long it is; nor need a 304, whose length would have
    // to be the 200's (RFC 9110, section 8.6).
    const bool has_content =
        !status_is_information(status_code) && status_code != status::NO_CONTENT && status_code != status::NOT_MODIFIED;

    resp->status_code = status_code;
    if (has_content) resp->headers.set_content_length(content_length);
    else resp->headers.remove("Content-Length"sv);
    auto res = co_await encode(writer, *resp);
    if (!res.has_value())
    {
        // e.g. a header that can't be written (one with a line break in it), which nothing was written for.
        resp->status_code = status::NONE;
        throw std::system_error{res.error().value(), res.error().category(), "failed to send response"};
    }

    body = io::limit_writer{resp->body, has_content ? content_length : 0};
    co_return &body;
}

}
