#include "http/http11.hpp"

#include <cstddef>
#include <optional>

#include "http/headers.hpp"
#include "http/http.hpp"
#include "io/buffered_reader.hpp"
#include "util/result.hpp"
#include "util/string_util.hpp"

namespace
{

using namespace std::string_view_literals;

using net::url;
using net::http::client_response;
using net::http::headers;
using net::http::parse_method;
using net::http::parse_status;
using net::http::protocol_version;
using net::http::request;
using net::http::request_method;
using net::http::status;
using net::io::buffered_reader;
using net::util::result;
using net::util::split_string;
using net::util::trim_string;

result<uint32_t, std::errc> from_chars(std::string_view str) noexcept
{
    uint32_t value;
    auto [_, err] = std::from_chars(str.data(), str.data() + str.size(), value);

    if (static_cast<int>(err) != 0) return {err};
    return {value};
}

result<std::string, std::error_condition> readline(buffered_reader<char>& reader) noexcept
{
    std::string line;

    for (;;)
    {
        auto io_res = reader.ensure();
        if (io_res.err) return io_res.err;
        if (io_res.count == 0) return {""};

        auto result = reader.read_until<std::string>("\r\n"sv);
        if (result.has_value())
        {
            line.append(result.to_value());
            break;
        }

        auto add_len = result.to_error();
        if (add_len == 0)
        {
            // nothing more to read
            // TODO: need to better indicate that \r\n wasn't found...
            break;
        }

        auto old_len = line.size();
        line.resize(line.size() + add_len);
        reader.read(line.data() + old_len, add_len);
    }

    return line;
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

std::error_condition parse_status_line(buffered_reader<char>& reader, client_response& resp) noexcept
{
    // TODO: max bytes to read

    auto result = readline(reader);
    if (result.has_error()) return result.to_error();

    auto line = result.to_value();
    auto view = static_cast<std::string_view>(line);

    auto version_end = view.find(' ');
    if (version_end == std::string::npos) return std::make_error_condition(std::errc::illegal_byte_sequence);

    auto res = parse_http_version(view.substr(0, version_end));
    if (res.has_error()) return res.to_error();
    resp.version = res.to_value();

    auto status_start = version_end + 1;
    auto status_end   = view.find(' ', status_start);
    if (status_end == std::string::npos) return std::make_error_condition(std::errc::illegal_byte_sequence);

    resp.status_code = parse_status(view.substr(status_start, status_end));
    if (resp.status_code == status::NONE) return std::make_error_condition(std::errc::illegal_byte_sequence);

    // we can just ignore the reason

    return {};
}

std::error_condition parse_request_line(buffered_reader<char>& reader, request& req) noexcept
{
    // TODO: max bytes to read

    auto result = readline(reader);
    if (result.has_error()) return result.to_error();

    auto line = result.to_value();
    auto view = static_cast<std::string_view>(line);

    auto method_end = view.find(' ');
    if (method_end == std::string::npos) return std::make_error_condition(std::errc::illegal_byte_sequence);

    req.method = parse_method(view.substr(0, method_end));
    if (req.method == request_method::NONE) return std::make_error_condition(std::errc::illegal_byte_sequence);

    auto uri_start = method_end + 1;
    auto uri_end   = view.find(' ', uri_start);
    if (uri_end == std::string::npos) return std::make_error_condition(std::errc::illegal_byte_sequence);

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

std::error_condition parse_headers(buffered_reader<char>& reader, size_t max_read, headers& headers) noexcept
{
    size_t      amount_read = 0;
    std::string line;

    while (amount_read < max_read)
    {
        auto ensure_amount = std::min(max_read - amount_read, reader.capacity());
        auto [_, err]      = reader.ensure(ensure_amount);
        if (err) return err;

        auto next_eol = reader.find("\r\n"sv).value_or(0);
        if (next_eol == 0)
        {
            // blank line - we're done
            break;
        }

        // consume the newline characters too
        line.resize(next_eol + 2);
        reader.read(line.data(), line.size());

        amount_read += line.size();

        auto split_idx = line.find(':');
        if (split_idx == std::string::npos) continue; // invalid header

        auto view = static_cast<std::string_view>(line);

        auto key = trim_string(view.substr(0, split_idx));
        auto val = trim_string(view.substr(split_idx + 1));

        // TODO: validate key
        // TODO: check for list values

        headers.set(key, val);
    }

    return {};
}

}

namespace net::http::http11
{

using util::result;

std::error_condition request_encode(io::writer<char>& writer, const request& req) noexcept
{
    // TODO
}

std::error_condition response_encode(io::writer<char>& writer, const server_response& resp) noexcept
{
    // TODO
}

std::error_condition read_headers(io::buffered_reader<char>& reader, request& req, size_t max_read) noexcept
{
    if (auto err = parse_request_line(reader, req); err) return err;
    if (auto err = parse_headers(reader, max_read, req.headers); err) return err;

    return {};
}

std::error_condition read_headers(io::buffered_reader<char>& reader, client_response& resp, size_t max_read) noexcept
{
    if (auto err = parse_status_line(reader, resp); err) return err;
    if (auto err = parse_headers(reader, max_read, resp.headers); err) return err;

    return {};
}

result<request, std::error_condition> request_decode(io::reader<char>& reader, size_t max_header_bytes) noexcept
{
    auto buffer = std::make_unique<io::buffered_reader<char>>(reader);

    request req;

    if (auto err = parse_request_line(*buffer, req); err) return {err};
    if (auto err = parse_headers(*buffer, max_header_bytes, req.headers); err) return {err};

    // TODO wrap the body up to correctly identify "end of request"
    req.body = std::move(buffer);
    return {std::move(req)};
}

result<client_response, std::error_condition> response_decode(io::reader<char>& reader,
                                                              size_t            max_header_bytes) noexcept
{
    auto buffer = std::make_unique<io::buffered_reader<char>>(reader);

    client_response resp;

    if (auto err = parse_status_line(*buffer, resp); err) return {err};
    if (auto err = parse_headers(*buffer, max_header_bytes, resp.headers); err) return {err};

    // TODO wrap the body up to correctly identify "end of request"
    resp.body = std::move(buffer);
    return {std::move(resp)};
}

}
