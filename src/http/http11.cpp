#include "http/http11.hpp"

#include <cstddef>

#include "http/http.hpp"
#include "io/buffered_reader.hpp"
#include "util/result.hpp"
#include "util/string_util.hpp"

namespace
{

using net::url;
using net::http::client_response;
using net::http::headers_map;
using net::http::parse_method;
using net::http::parse_status;
using net::http::protocol_version;
using net::http::request;
using net::http::request_method;
using net::http::server_response;
using net::http::status;
using net::io::buffered_reader;
using net::util::result;
using net::util::trim_string;

result<uint32_t, std::errc> from_chars(std::string_view str) noexcept
{
    uint32_t value;
    auto [_, err] = std::from_chars(str.data(), str.data() + str.size(), value);

    if (static_cast<int>(err) != 0) return {err};
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

    protocol_version version{
        .major = major_res.to_value(),
        .minor = minor_res.to_value(),
    };

    return {version};
}

std::error_condition parse_status_line(buffered_reader<char>& reader, client_response& resp) noexcept
{
    using namespace std::string_view_literals;

    std::string line;

    // TODO: pull out into a helper function
    for (;;)
    {
        auto io_res = reader.ensure();
        if (io_res.err) return io_res.err;

        auto result = reader.read_until("\r\n"sv);
        if (result.has_value())
        {
            line.append(result.to_value());
            break;
        }

        auto old_len = line.size();
        auto add_len = result.to_error();

        line.resize(line.size() + add_len);
        reader.read(line.data() + old_len, add_len);
    }

    /* if (std::getline(reader, line).bad()) return {std::make_error_condition(std::errc::illegal_byte_sequence)}; */

    auto view = static_cast<std::string_view>(line);

    auto version_end = view.find(' ');
    if (version_end == std::string::npos) return {std::make_error_condition(std::errc::illegal_byte_sequence)};

    auto res = parse_http_version(view.substr(0, version_end));
    if (res.has_error()) return {res.to_error()};
    resp.version = res.to_value();

    auto status_start = version_end + 1;
    auto status_end   = view.find(' ', status_start);
    if (status_end == std::string::npos) return {std::make_error_condition(std::errc::illegal_byte_sequence)};

    resp.status_code = parse_status(view.substr(status_start, status_end));
    if (resp.status_code == status::NONE) return {std::make_error_condition(std::errc::illegal_byte_sequence)};

    // we can just ignore the reason

    return {};
}

std::error_condition parse_request_line(buffered_reader<std::byte>& reader, request& req) noexcept
{
    std::string line;
    /* if (std::getline(reader, line).bad()) return {std::make_error_condition(std::errc::illegal_byte_sequence)}; */

    auto view = static_cast<std::string_view>(line);

    auto method_end = view.find(' ');
    if (method_end == std::string::npos) return {std::make_error_condition(std::errc::illegal_byte_sequence)};

    req.method = parse_method(view.substr(0, method_end));
    if (req.method == request_method::NONE) return {std::make_error_condition(std::errc::illegal_byte_sequence)};

    auto uri_start = method_end + 1;
    auto uri_end   = view.find(' ', uri_start);
    if (uri_end == std::string::npos) return {std::make_error_condition(std::errc::illegal_byte_sequence)};

    auto uri = view.substr(uri_start, uri_end - uri_start);
    if (uri == "*") req.uri.host = "*";
    else
        url::parse(uri) //
            .if_value([&](const url& u) { req.uri = u; })
            .if_error([&](auto) { req.uri.path = "/"; });

    auto res = parse_http_version(view.substr(uri_end + 1));
    if (res.has_error()) return {res.to_error()};
    req.version = res.to_value();

    return {};
}

void parse_headers(headers_map& headers, buffered_reader<std::byte>& reader) noexcept
{
    std::string line;
    /* while (std::getline(reader, line)) */
    while (false)
    {
        if (line.empty()) break;

        auto split_idx = line.find(':');
        if (split_idx == std::string::npos) continue; // invalid header

        auto view = static_cast<std::string_view>(line);

        auto key = view.substr(0, split_idx);
        auto val = trim_string(view.substr(split_idx + 1));
        headers[std::string{key}].emplace_back(val);
    }
}

}

namespace net::http::http11
{

using util::result;

std::error_condition request_encode(io::writer<std::byte>& writer, const request& req) noexcept
{
    // TODO
}

std::error_condition response_encode(io::writer<std::byte>& writer, const server_response& resp) noexcept
{
    // TODO
}

result<request, std::error_condition> request_decode(io::reader<std::byte>& reader) noexcept
{
    io::buffered_reader buffer(reader);

    request req{
        .body = buffer,
    };

    auto err = parse_request_line(buffer, req);
    if (err) return {err};

    parse_headers(req.headers, buffer);

    // TODO wrap the body up to correctly identify "EOF"
    return {req};
}

result<client_response, std::error_condition> response_decode(io::reader<std::byte>& reader) noexcept
{
    io::buffered_reader buffer(reader);

    client_response resp{
        .body = buffer,
    };

    auto err = parse_status_line(buffer, resp);
    if (err) return {err};

    parse_headers(resp.headers, buffer);

    // TODO wrap the body up to correctly identify "EOF"
    return {resp};
}

}
