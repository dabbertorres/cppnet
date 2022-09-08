#include "http/http11.hpp"

#include "http/http.hpp"
#include "result.hpp"

namespace
{

using namespace std::string_view_literals;

using namespace net;
using namespace net::http;

using util::result;

// source https://en.wikipedia.org/wiki/Whitespace_character#Unicode
static constexpr auto whitespace =
    " \b\f\n\r\t\v\u2000\u2001\u2002\u2003\u2004\u2005\u2006\u2007\u2008\u2009\u200A\u2028\u2029\u202f\u205f\u3000"sv;

static std::string_view trim_string(std::string_view str) noexcept
{
    auto first_not_space = str.find_first_not_of(whitespace);
    auto last_not_space  = str.find_last_not_of(whitespace);

    str.remove_suffix(str.size() - (last_not_space + 1));
    str.remove_prefix(first_not_space);
    return str;
}

static std::errc from_chars(std::string_view str, uint32_t& value) noexcept
{
    auto [_, err] = std::from_chars(str.data(), str.data() + str.size(), value);
    return err;
}

static result<version, std::error_condition> parse_http_version(std::string_view str)
{
    static constexpr auto name = "HTTP/"sv;

    if (!str.starts_with(name))
        return {std::make_error_condition(std::errc::illegal_byte_sequence)};

    str.remove_prefix(name.size());

    auto split_idx = str.find('.');
    if (split_idx == std::string_view::npos)
        return {std::make_error_condition(std::errc::illegal_byte_sequence)};

    version v;

    if (auto err = from_chars(str.substr(0, split_idx), v.major); err != std::errc{})
        return {std::make_error_condition(err)};

    if (auto err = from_chars(str.substr(split_idx + 1), v.minor); err != std::errc{})
        return {std::make_error_condition(err)};

    return {v};
}

static std::error_condition parse_status_line(std::istream& in, response& resp) noexcept
{
    std::string line;
    if (std::getline(in, line).bad())
        return {std::make_error_condition(std::errc::illegal_byte_sequence)};

    auto view = static_cast<std::string_view>(line);

    auto version_end = view.find(' ');
    if (version_end == std::string::npos)
        return {std::make_error_condition(std::errc::illegal_byte_sequence)};

    auto res = parse_http_version(view.substr(0, version_end));
    if (res.has_error())
        return {res.to_error()};
    else
        resp.version = res.to_value();

    auto status_start = version_end + 1;
    auto status_end   = view.find(' ', status_start);
    if (status_end == std::string::npos)
        return {std::make_error_condition(std::errc::illegal_byte_sequence)};

    resp.status = parse_status(view.substr(status_start, status_end));
    if (resp.status == status::NONE)
        return {std::make_error_condition(std::errc::illegal_byte_sequence)};

    // we can just ignore the reason

    return {};
}

static std::error_condition parse_request_line(std::istream& in, request& req) noexcept
{
    std::string line;
    if (std::getline(in, line).bad())
        return {std::make_error_condition(std::errc::illegal_byte_sequence)};

    auto view = static_cast<std::string_view>(line);

    auto method_end = view.find(' ');
    if (method_end == std::string::npos)
        return {std::make_error_condition(std::errc::illegal_byte_sequence)};

    req.method = parse_method(view.substr(0, method_end));
    if (req.method == method::NONE)
        return {std::make_error_condition(std::errc::illegal_byte_sequence)};

    auto uri_start = method_end + 1;
    auto uri_end   = view.find(' ', uri_start);
    if (uri_end == std::string::npos)
        return {std::make_error_condition(std::errc::illegal_byte_sequence)};

    auto uri = view.substr(uri_start, uri_end - uri_start);
    if (uri == "*")
        req.uri.host = "*";
    else
        url::parse(uri) //
            .if_value([&](const url& u) { req.uri = u; })
            .if_error([&](auto) { req.uri.path = "/"; });

    if (auto res = parse_http_version(view.substr(uri_end + 1)); res.has_error())
        return {res.to_error()};
    else
        req.version = res.to_value();

    return {};
}

static void parse_headers(headers& headers, std::istream& in) noexcept
{
    std::string line;
    while (std::getline(in, line))
    {
        if (line == "") break;

        auto split_idx = line.find(':');
        if (split_idx == std::string::npos) continue; // invalid header

        auto key = line.substr(0, split_idx);
        auto val = trim_string(line.substr(split_idx + 1));
        headers[std::string{key}].emplace_back(val);
    }
}

}

namespace net::http::http11
{

using util::result;

std::error_condition request_encode(std::ostream& out, const request& in) noexcept
{
    // TODO
}

std::error_condition response_encode(std::ostream& out, const response& resp) noexcept
{
    // TODO
}

result<request, std::error_condition> request_decode(std::istream& in) noexcept
{
    request req{
        .body = in,
    };

    auto err = parse_request_line(in, req);
    if (err) return {err};

    parse_headers(req.headers, in);

    // TODO wrap the body up to correctly identify "EOF"
    return {req};
}

result<response, std::error_condition> response_decode(std::istream& in) noexcept
{
    response resp{
        .body = in,
    };

    auto err = parse_status_line(in, resp);
    if (err) return {err};

    parse_headers(resp.headers, in);

    // TODO wrap the body up to correctly identify "EOF"
    return {resp};
}

}
