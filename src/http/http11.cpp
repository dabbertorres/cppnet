#include "http/http11.hpp"

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <spdlog/spdlog.h>

#include "coro/task.hpp"
#include "http/chunked_reader.hpp"
#include "http/headers.hpp"
#include "http/http.hpp"
#include "http/request.hpp"
#include "http/response.hpp"
#include "io/buffered_reader.hpp"
#include "io/io.hpp"
#include "io/limit_reader.hpp"
#include "io/util.hpp"
#include "io/writer.hpp"
#include "util/result.hpp"
#include "util/string_util.hpp"

#include "url.hpp"

namespace
{

using namespace std::string_view_literals;

using net::url;
using net::coro::task;
using net::http::client_response;
using net::http::headers;
using net::http::parse_method;
using net::http::parse_status;
using net::http::protocol_version;
using net::http::request_method;
using net::http::server_request;
using net::http::status;
using net::io::buffered_reader;
using net::util::result;
using net::util::split_string;
using net::util::trim_string;

result<std::uint32_t, std::errc> from_chars(std::string_view str) noexcept
{
    std::uint32_t value; // NOLINT(cppcoreguidelines-init-variables)
    auto [_, err] = std::from_chars(str.data(), str.data() + str.size(), value);

    if (err != std::errc()) return {err};
    return {value};
}

result<protocol_version, std::error_condition> parse_http_version(std::string_view str)
{
    using namespace std::literals::string_view_literals;

    static constexpr auto name = "HTTP/"sv;

    if (!str.starts_with(name)) return {std::make_error_condition(std::errc::illegal_byte_sequence)};

    str.remove_prefix(name.size());

    auto split_idx = str.find('.');
    if (split_idx == std::string_view::npos) return {std::make_error_condition(std::errc::illegal_byte_sequence)};

    auto major_res = from_chars(str.substr(0, split_idx));
    if (major_res.has_error()) return {std::make_error_condition(major_res.to_error())};

    auto minor_res = from_chars(str.substr(split_idx + 1));
    if (minor_res.has_error()) return {std::make_error_condition(minor_res.to_error())};

    const protocol_version version{
        .major = major_res.to_value(),
        .minor = minor_res.to_value(),
    };

    return {version};
}

task<std::error_condition> parse_status_line(buffered_reader* reader, client_response& resp) noexcept
{
    // TODO: max bytes to read

    auto result = readline(reader);
    if (result.has_error()) co_return result.to_error();

    auto line = result.to_value();
    if (line.empty()) co_return make_error_condition(net::io::status_condition::closed);
    auto view = static_cast<std::string_view>(line);

    auto version_end = view.find(' ');
    if (version_end == std::string_view::npos) co_return std::make_error_condition(std::errc::illegal_byte_sequence);

    auto res = parse_http_version(view.substr(0, version_end));
    if (res.has_error()) co_return res.to_error();
    resp.version = res.to_value();

    auto status_start = version_end + 1;
    auto status_end   = view.find(' ', status_start);
    if (status_end == std::string_view::npos) co_return std::make_error_condition(std::errc::illegal_byte_sequence);

    resp.status_code = parse_status(view.substr(status_start, status_end));
    if (resp.status_code == status::NONE) co_return std::make_error_condition(std::errc::illegal_byte_sequence);

    // we can just ignore the reason

    co_return std::error_condition{};
}

task<std::error_condition> parse_request_line(buffered_reader* reader, server_request& req) noexcept
{
    // TODO: max bytes to read

    auto result = co_await co_readline(reader);
    if (result.has_error()) co_return result.to_error();

    auto line = result.to_value();
    if (line.empty()) co_return make_error_condition(net::io::status_condition::closed);
    auto view = static_cast<std::string_view>(line);

    auto method_end = view.find(' ');
    if (method_end == std::string_view::npos) co_return std::make_error_condition(std::errc::illegal_byte_sequence);

    req.method = parse_method(view.substr(0, method_end));
    if (req.method == request_method::NONE) co_return std::make_error_condition(std::errc::illegal_byte_sequence);

    auto uri_start = method_end + 1;
    auto uri_end   = view.find(' ', uri_start);
    if (uri_end == std::string_view::npos) co_return std::make_error_condition(std::errc::illegal_byte_sequence);

    auto uri = view.substr(uri_start, uri_end - uri_start);

    // clang-format off
    if (uri == "*")
        req.uri.host = "*";
    else
        url::parse(uri)
            .if_value([&](const url& u) { req.uri = u; })
            .if_error([&](auto) { req.uri.path = "/"; });
    // clang-format on

    auto res = parse_http_version(view.substr(uri_end + 1));
    if (res.has_error()) co_return res.to_error();
    req.version = res.to_value();

    co_return std::error_condition{};
}

task<std::error_condition> parse_headers(buffered_reader* reader, std::size_t max_read, headers& headers) noexcept
{
    std::size_t amount_read = 0;

    while (amount_read < max_read)
    {
        auto maybe_line = co_await co_readline(reader);
        if (!maybe_line.has_value()) co_return maybe_line.to_error();

        auto next_line = maybe_line.to_value();
        if (next_line.length() == 0)
        {
            // blank line - we're done
            co_return std::error_condition{};
        }

        amount_read += next_line.size();

        auto split_idx = next_line.find(':');
        if (split_idx == std::string::npos) continue; // invalid header

        auto view = static_cast<std::string_view>(next_line);

        auto key = trim_string(view.substr(0, split_idx));
        auto val = trim_string(view.substr(split_idx + 1));

        // TODO: validate key

        for (auto v : split_string(val, ','))
        {
            headers.add(key, v);
        }
    }

    co_return std::make_error_condition(std::errc::value_too_large);
}

task<std::error_condition> write_headers(net::io::writer& writer, const headers& headers) noexcept
{
    for (const auto& header : headers)
    {
        auto res = co_await writer.co_write(header.first);
        if (res.err) co_return res.err;

        res = co_await writer.co_write(": "sv);
        if (res.err) co_return res.err;

        if (!header.second.empty())
        {
            res = co_await writer.co_write(header.second.front());
            if (res.err) co_return res.err;

            for (const auto& value : std::ranges::drop_view(header.second, 1))
            {
                res = co_await writer.co_write(", "sv);
                if (res.err) co_return res.err;

                res = co_await writer.co_write(value);
                if (res.err) co_return res.err;
            }
        }

        res = co_await writer.co_write("\r\n"sv);
        if (res.err) co_return res.err;
    }

    co_return std::error_condition{};
}

}

namespace net::http::http11
{

using coro::task;
using util::result;

task<result<io::writer*, std::error_condition>> request_encode(io::writer* writer, const client_request& req) noexcept
{
    using result_t = result<io::writer*, std::error_condition>;

    auto res = co_await writer->co_write(method_string(req.method));
    if (res.err) co_return result_t{res.err};

    res = co_await writer->co_write(' ');
    if (res.err) co_return result_t{res.err};

    // TODO: don't build the uri before writing it
    res = co_await writer->co_write(req.uri.build());
    if (res.err) co_return result_t{res.err};

    res = co_await writer->co_write(" HTTP/"sv);
    if (res.err) co_return result_t{res.err};

    auto major_version = req.version.major;
    auto minor_version = req.version.minor;
    if (major_version == 0 && minor_version == 0)
    {
        major_version = 1;
        minor_version = 0;
    }

    std::array<char, 4> version_buf{
        static_cast<char>(major_version + '0'),
        '.',
        static_cast<char>(minor_version + '0'),
        ' ',
    };

    res = co_await writer->co_write(version_buf);
    if (res.err) co_return result_t{res.err};

    res = co_await writer->co_write("\r\n"sv);
    if (res.err) co_return result_t{res.err};

    res.err = co_await write_headers(*writer, req.headers);
    if (res.err) co_return result_t{res.err};

    res = co_await writer->co_write("\r\n"sv);
    if (res.err) co_return result_t{res.err};

    // TODO: copy req.body to writer

    co_return result_t{writer};
}

task<result<io::writer*, std::error_condition>> response_encode(io::writer*            writer,
                                                                const server_response& resp) noexcept
{
    using result_t = result<io::writer*, std::error_condition>;

    auto res = co_await writer->co_write("HTTP/"sv);
    if (res.err) co_return result_t{res.err};

    std::array<char, 4> version_buf{
        static_cast<char>(resp.version.major + '0'),
        '.',
        static_cast<char>(resp.version.minor + '0'),
        ' ',
    };

    res = co_await writer->co_write(version_buf);
    if (res.err) co_return result_t{res.err};

    res = co_await writer->co_write(status_text(resp.status_code));
    if (res.err) co_return result_t{res.err};

    res = co_await writer->co_write("\r\n"sv);
    if (res.err) co_return result_t{res.err};

    res.err = co_await write_headers(*writer, resp.headers);
    if (res.err) co_return result_t{res.err};

    res = co_await writer->co_write("\r\n"sv);
    if (res.err) co_return result_t{res.err};

    co_return result_t{writer};
}

task<result<server_request, std::error_condition>> request_decode(std::unique_ptr<io::buffered_reader> reader,
                                                                  std::size_t max_header_bytes) noexcept
{
    using result_t = result<server_request, std::error_condition>;

    server_request req;

    auto err = co_await parse_request_line(reader.get(), req);
    if (err) co_return result_t{err};

    err = co_await parse_headers(reader.get(), max_header_bytes, req.headers);
    if (err) co_return result_t{err};

    if (req.uri.host.empty()) req.uri.host = req.headers.get("Host"sv).value_or(""sv);

    if (req.headers.is_chunked())
    {
        req.body = std::make_unique<chunked_reader>(std::move(reader));
    }
    else
    {
        std::size_t content_length = req.headers.get_content_length().value_or(0);
        req.body                   = std::make_unique<io::limit_reader>(std::move(reader), content_length);
    }

    // TODO: trailers

    co_return result_t{std::move(req)};
}

coro::task<result<client_response, std::error_condition>> response_decode(std::unique_ptr<io::buffered_reader> reader,
                                                                          std::size_t max_header_bytes) noexcept
{
    using result_t = result<client_response, std::error_condition>;

    client_response resp;

    if (auto err = co_await parse_status_line(reader.get(), resp); err) co_return result_t{err};
    if (auto err = co_await parse_headers(reader.get(), max_header_bytes, resp.headers); err) co_return result_t{err};

    if (resp.headers.is_chunked())
    {
        resp.body = std::make_unique<chunked_reader>(std::move(reader));
    }
    else
    {
        const std::size_t content_length = resp.headers.get_content_length().value_or(0);
        resp.body                        = std::make_unique<io::limit_reader>(std::move(reader), content_length);
    }

    // TODO: trailers

    co_return result_t{std::move(resp)};
}

}
