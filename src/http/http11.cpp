#include "http/http11.hpp"

#include "http/http.hpp"

#include "result.hpp"
#include "string_util.hpp"

namespace
{

using net::url;
using net::http::headers;
using net::http::parse_method;
using net::http::parse_status;
using net::http::protocol_version;
using net::http::request;
using net::http::request_method;
using net::http::response;
using net::http::status;
using net::util::result;
using net::util::trim_string;

std::errc from_chars(std::string_view str, uint32_t& value) noexcept
{
    auto [_, err] = std::from_chars(str.data(), str.data() + str.size(), value);
    return err;
}

result<protocol_version, std::error_condition> parse_http_version(std::string_view str)
{
    using namespace std::literals::string_view_literals;

    static constexpr auto name = "HTTP/"sv;

    if (!str.starts_with(name)) return {std::make_error_condition(std::errc::illegal_byte_sequence)};

    str.remove_prefix(name.size());

    auto split_idx = str.find('.');
    if (split_idx == std::string_view::npos) return {std::make_error_condition(std::errc::illegal_byte_sequence)};

    protocol_version v;

    if (auto err = from_chars(str.substr(0, split_idx), v.major); err != std::errc{})
        return {std::make_error_condition(err)};

    if (auto err = from_chars(str.substr(split_idx + 1), v.minor); err != std::errc{})
        return {std::make_error_condition(err)};

    return {v};
}

std::error_condition parse_status_line(std::istream& in, response& resp) noexcept
{
    std::string line;
    if (std::getline(in, line).bad()) return {std::make_error_condition(std::errc::illegal_byte_sequence)};

    auto view = static_cast<std::string_view>(line);

    auto version_end = view.find(' ');
    if (version_end == std::string::npos) return {std::make_error_condition(std::errc::illegal_byte_sequence)};

    auto res = parse_http_version(view.substr(0, version_end));
    if (res.has_error()) return {res.to_error()};
    resp.version = res.to_value();

    auto status_start = version_end + 1;
    auto status_end   = view.find(' ', status_start);
    if (status_end == std::string::npos) return {std::make_error_condition(std::errc::illegal_byte_sequence)};

    resp.status = parse_status(view.substr(status_start, status_end));
    if (resp.status == status::NONE) return {std::make_error_condition(std::errc::illegal_byte_sequence)};

    // we can just ignore the reason

    return {};
}

std::error_condition parse_request_line(std::istream& in, request& req) noexcept
{
    std::string line;
    if (std::getline(in, line).bad()) return {std::make_error_condition(std::errc::illegal_byte_sequence)};

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

void parse_headers(headers& headers, std::istream& in) noexcept
{
    std::string line;
    while (std::getline(in, line))
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

std::error_condition request_encode(std::ostream& w, const request& req) noexcept
{
    // TODO
}

std::error_condition response_encode(std::ostream& w, const response& resp) noexcept
{
    // TODO
}

result<request, std::error_condition> request_decode(std::istream& r) noexcept
{
    request req{
        .body = r,
    };

    auto err = parse_request_line(r, req);
    if (err) return {err};

    parse_headers(req.headers, r);

    // TODO wrap the body up to correctly identify "EOF"
    return {req};
}

result<response, std::error_condition> response_decode(std::istream& r) noexcept
{
    response resp{
        .body = r,
    };

    auto err = parse_status_line(r, resp);
    if (err) return {err};

    parse_headers(resp.headers, r);

    // TODO wrap the body up to correctly identify "EOF"
    return {resp};
}

}
