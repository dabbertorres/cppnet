#include "http/response.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "http/headers.hpp"
#include "io/writer.hpp"

namespace net::http
{

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

io::writer& response_writer::send(status status_code, std::size_t content_length)
{
    resp->status_code = status_code;
    resp->headers.set_content_length(content_length);
    encode(writer, *resp);
    return *resp->body;
}

}
