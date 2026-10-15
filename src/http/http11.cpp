#include "http/http11.hpp"

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
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
#include "http/request_target.hpp"
#include "http/response.hpp"
#include "io/buffered_reader.hpp"
#include "io/io.hpp"
#include "io/limit_reader.hpp"
#include "io/util.hpp"
#include "io/writer.hpp"
#include "url.hpp"
#include "util/result.hpp"
#include "util/string_util.hpp"

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
using net::http::parse_request_target;
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

    auto result = co_await readline(reader);
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

    auto result = co_await readline(reader);
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

    auto target = parse_request_target(req.method, view.substr(uri_start, uri_end - uri_start));
    if (!target.has_value()) co_return target.error();
    req.target = std::move(*target);

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
        auto maybe_line = co_await readline(reader);
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

bool is_default_port(std::string_view scheme, std::string_view port) noexcept
{
    return (port == "80"sv && (scheme.empty() || net::util::equal_ignore_case(scheme, "http"sv)))
        || (port == "443"sv && net::util::equal_ignore_case(scheme, "https"sv));
}

task<std::error_condition> write_headers(net::io::writer& writer, const headers& headers) noexcept
{
    for (const auto& header : headers)
    {
        auto res = co_await writer.write(header.first);
        if (res.err) co_return res.err;

        res = co_await writer.write(": "sv);
        if (res.err) co_return res.err;

        if (!header.second.empty())
        {
            res = co_await writer.write(header.second.front());
            if (res.err) co_return res.err;

            for (const auto& value : std::ranges::drop_view(header.second, 1))
            {
                res = co_await writer.write(", "sv);
                if (res.err) co_return res.err;

                res = co_await writer.write(value);
                if (res.err) co_return res.err;
            }
        }

        res = co_await writer.write("\r\n"sv);
        if (res.err) co_return res.err;
    }

    co_return std::error_condition{};
}

}

namespace net::http::http11
{

using coro::task;

task<request_encoder_result> request_encode(io::writer* writer, const client_request& req) noexcept
{
    auto res = co_await writer->write(method_string(req.method));
    if (res.err) co_return std::unexpected(res.err);

    res = co_await writer->write(' ');
    if (res.err) co_return std::unexpected(res.err);

    // the target is written in origin-form ("/path?query"), encoded so that nothing in the url can end the line.
    res = co_await writer->write(origin_form(req.uri));
    if (res.err) co_return std::unexpected(res.err);

    res = co_await writer->write(" HTTP/"sv);
    if (res.err) co_return std::unexpected(res.err);

    auto major_version = req.version.major;
    auto minor_version = req.version.minor;
    if (major_version == 0 && minor_version == 0)
    {
        major_version = 1;
        minor_version = 0;
    }

    // NOTE: unlike the status line, nothing follows the version.
    std::array<char, 3> version_buf{
        static_cast<char>(major_version + '0'),
        '.',
        static_cast<char>(minor_version + '0'),
    };

    res = co_await writer->write(version_buf);
    if (res.err) co_return std::unexpected(res.err);

    res = co_await writer->write("\r\n"sv);
    if (res.err) co_return std::unexpected(res.err);

    // HTTP/1.1 requires a Host header.
    if (!req.headers.get("Host"sv).has_value() && !req.uri.host.empty())
    {
        auto host_line = "Host: " + req.uri.host;
        if (!req.uri.port.empty() && !is_default_port(req.uri.scheme, req.uri.port)) host_line += ":" + req.uri.port;
        host_line += "\r\n";

        res = co_await writer->write(host_line);
        if (res.err) co_return std::unexpected(res.err);
    }

    res.err = co_await write_headers(*writer, req.headers);
    if (res.err) co_return std::unexpected(res.err);

    res = co_await writer->write("\r\n"sv);
    if (res.err) co_return std::unexpected(res.err);

    // TODO: copy req.body to writer

    co_return writer;
}

task<response_encoder_result> response_encode(io::writer* writer, const server_response& resp) noexcept
{
    auto res = co_await writer->write("HTTP/"sv);
    if (res.err) co_return std::unexpected(res.err);

    std::array<char, 4> version_buf{
        static_cast<char>(resp.version.major + '0'),
        '.',
        static_cast<char>(resp.version.minor + '0'),
        ' ',
    };

    res = co_await writer->write(version_buf);
    if (res.err) co_return std::unexpected(res.err);

    res = co_await writer->write(status_text(resp.status_code));
    if (res.err) co_return std::unexpected(res.err);

    res = co_await writer->write("\r\n"sv);
    if (res.err) co_return std::unexpected(res.err);

    res.err = co_await write_headers(*writer, resp.headers);
    if (res.err) co_return std::unexpected(res.err);

    res = co_await writer->write("\r\n"sv);
    if (res.err) co_return std::unexpected(res.err);

    co_return writer;
}

task<request_decoder_result> request_decode(io::buffered_reader* reader, std::size_t max_header_bytes) noexcept
{
    server_request req;

    auto err = co_await parse_request_line(reader, req);
    if (err) co_return std::unexpected(err);

    err = co_await parse_headers(reader, max_header_bytes, req.headers);
    if (err) co_return std::unexpected(err);

    // RFC 9112, section 3.2.2: a target with a host overrides the Host header.
    req.host = !req.target.host.empty() ? req.target.host : std::string{req.headers.get("Host"sv).value_or(""sv)};

    if (req.headers.is_chunked())
    {
        req.body = std::make_unique<chunked_reader>(static_cast<io::reader*>(reader));
    }
    else
    {
        std::size_t content_length = req.headers.get_content_length().value_or(0);
        req.body = std::make_unique<io::limit_reader>(static_cast<io::reader*>(reader), content_length);
    }

    // TODO: trailers

    co_return req;
}

coro::task<response_decoder_result> response_decode(std::unique_ptr<io::buffered_reader> reader,
                                                    std::size_t                          max_header_bytes) noexcept
{
    client_response resp;

    if (auto err = co_await parse_status_line(reader.get(), resp); err) co_return std::unexpected(err);
    if (auto err = co_await parse_headers(reader.get(), max_header_bytes, resp.headers); err)
        co_return std::unexpected(err);

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

    co_return resp;
}

}
