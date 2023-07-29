#include "http/http11.hpp"

#include <charconv>
#include <cstddef>
#include <ranges>
#include <system_error>

#include "http/headers.hpp"
#include "http/http.hpp"
#include "http/request.hpp"
#include "http/response.hpp"
#include "io/buffered_reader.hpp"
#include "io/limit_reader.hpp"
#include "io/util.hpp"
#include "util/result.hpp"
#include "util/string_util.hpp"

namespace
{

using namespace std::string_view_literals;

using net::url;
using net::http::client_request;
using net::http::client_response;
using net::http::headers;
using net::http::parse_method;
using net::http::parse_status;
using net::http::protocol_version;
using net::http::request_method;
using net::http::server_request;
using net::http::server_response;
using net::http::status;
using net::io::buffered_reader;
using net::util::result;
using net::util::trim_string;

result<uint32_t, std::errc> from_chars(std::string_view str) noexcept
{
    uint32_t value; // NOLINT(cppcoreguidelines-init-variables)
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

std::error_condition parse_status_line(buffered_reader& reader, client_response& resp) noexcept
{
    // TODO: max bytes to read

    auto result = net::io::readline(reader);
    if (result.has_error()) return result.to_error();

    auto line = result.to_value();
    auto view = static_cast<std::string_view>(line);

    auto version_end = view.find(' ');
    if (version_end == std::string_view::npos) return std::make_error_condition(std::errc::illegal_byte_sequence);

    auto res = parse_http_version(view.substr(0, version_end));
    if (res.has_error()) return res.to_error();
    resp.version = res.to_value();

    auto status_start = version_end + 1;
    auto status_end   = view.find(' ', status_start);
    if (status_end == std::string_view::npos) return std::make_error_condition(std::errc::illegal_byte_sequence);

    resp.status_code = parse_status(view.substr(status_start, status_end));
    if (resp.status_code == status::NONE) return std::make_error_condition(std::errc::illegal_byte_sequence);

    // we can just ignore the reason

    return {};
}

std::error_condition parse_request_line(buffered_reader& reader, server_request& req) noexcept
{
    // TODO: max bytes to read

    auto result = readline(reader);
    if (result.has_error()) return result.to_error();

    auto line = result.to_value();
    if (line.empty()) return std::make_error_condition(std::errc::io_error); // TODO: return a not ready error
    auto view = static_cast<std::string_view>(line);

    auto method_end = view.find(' ');
    if (method_end == std::string_view::npos) return std::make_error_condition(std::errc::illegal_byte_sequence);

    req.method = parse_method(view.substr(0, method_end));
    if (req.method == request_method::NONE) return std::make_error_condition(std::errc::illegal_byte_sequence);

    auto uri_start = method_end + 1;
    auto uri_end   = view.find(' ', uri_start);
    if (uri_end == std::string_view::npos) return std::make_error_condition(std::errc::illegal_byte_sequence);

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
    if (res.has_error()) return {res.to_error()};
    req.version = res.to_value();

    return {};
}

std::error_condition parse_headers(buffered_reader& reader, std::size_t max_read, headers& headers) noexcept
{
    std::size_t amount_read = 0;

    while (amount_read < max_read)
    {
        auto maybe_line = readline(reader);
        if (!maybe_line.has_value()) return maybe_line.to_error();

        auto next_line = maybe_line.to_value();
        if (next_line.length() == 0)
        {
            // blank line - we're done
            return {};
        }

        amount_read += next_line.size();

        auto split_idx = next_line.find(':');
        if (split_idx == std::string::npos) continue; // invalid header

        auto view = static_cast<std::string_view>(next_line);

        auto key = trim_string(view.substr(0, split_idx));
        auto val = trim_string(view.substr(split_idx + 1));

        // TODO: validate key
        // TODO: check for list values

        headers.set(key, val);
    }

    return std::make_error_condition(std::errc::value_too_large);
}

std::error_condition write_headers(net::io::writer& writer, const headers& headers) noexcept
{
    for (const auto& header : headers)
    {
        auto res = writer.write(header.first);
        if (res.err) return res.err;

        res = writer.write(": ");
        if (res.err) return res.err;

        if (!header.second.empty())
        {
            res = writer.write(header.second.front());
            if (res.err) return res.err;

            for (const auto& value : std::ranges::drop_view(header.second, 1))
            {
                res = writer.write(", ");
                if (res.err) return res.err;

                res = writer.write(value);
                if (res.err) return res.err;
            }
        }

        res = writer.write("\r\n");
        if (res.err) return res.err;
    }

    return {};
}

}

namespace net::http::http11
{

using util::result;

util::result<io::writer*, std::error_condition> request_encode(io::writer* writer, const client_request& req) noexcept
{
    // TODO
    return {writer};
}

util::result<io::writer*, std::error_condition> response_encode(io::writer*            writer,
                                                                const server_response& resp) noexcept
{
    auto res = writer->write("HTTP/");
    if (res.err) return res.err;

    std::array<char, 4> version_buf{
        static_cast<char>(resp.version.major + '0'),
        '.',
        static_cast<char>(resp.version.minor + '0'),
        ' ',
    };

    res = writer->write(version_buf.data(), version_buf.size());
    if (res.err) return {res.err};

    res = writer->write(status_text(resp.status_code));
    if (res.err) return {res.err};

    res = writer->write("\r\n");
    if (res.err) return res.err;

    res.err = write_headers(*writer, resp.headers);
    if (res.err) return res.err;

    res = writer->write("\r\n");
    if (res.err) return res.err;

    return {writer};
}

result<server_request, std::error_condition> request_decode(io::buffered_reader& reader,
                                                            size_t               max_header_bytes) noexcept
{
    server_request req;

    auto err = parse_request_line(reader, req);
    if (err) return {err};

    err = parse_headers(reader, max_header_bytes, req.headers);
    if (err) return {err};

    if (req.uri.host.empty()) req.uri.host = req.headers.get("Host"sv).value_or(""sv);

    std::size_t content_length = req.headers.get_content_length().value_or(0);
    req.body                   = io::limit_reader(&reader, content_length);
    return {req};
}

result<client_response, std::error_condition> response_decode(io::buffered_reader& reader,
                                                              std::size_t          max_header_bytes) noexcept
{
    client_response resp;

    if (auto err = parse_status_line(reader, resp); err) return {err};
    if (auto err = parse_headers(reader, max_header_bytes, resp.headers); err) return {err};

    const size_t content_length = resp.headers.get_content_length().value_or(0);
    resp.body                   = io::limit_reader(&reader, content_length);
    return {resp};
}

}
