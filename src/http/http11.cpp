#include "http/http11.hpp"

#include <istream>
#include <ostream>
#include <sstream>
#include <string>

#include "encoding.hpp"
#include "http/http.hpp"

namespace
{

// source https://en.wikipedia.org/wiki/Whitespace_character#Unicode
static constexpr auto whitespace =
    " \b\f\n\r\t\v\u2000\u2001\u2002\u2003\u2004\u2005\u2006\u2007\u2008\u2009\u200A\u2028\u2029\u202f\u205f\u3000";

static std::string_view trim_string(std::string_view s) noexcept
{
    auto first_not_space = s.find_first_not_of(whitespace);
    auto last_not_space  = s.find_last_not_of(whitespace);

    s.remove_suffix(s.size() - (last_not_space + 1));
    s.remove_prefix(first_not_space);
    return s;
}

}

namespace net::http::http11
{

using util::result;

std::error_condition response_encode(std::ostream& w, const response& r) noexcept
{
    // TODO
}

std::error_condition request_encode(std::ostream& w, const request& r) noexcept
{
    // TODO
}

result<response, std::error_condition> response_decode(std::istream& r) noexcept
{
    // TODO
}

result<request, std::error_condition> request_decode(std::istream& r) noexcept
{
    request req{
        .body = r,
    };

    std::string line;
    std::getline(r, line);

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
    {
        req.uri.host = "*";
    }
    else
    {
        url::parse(uri) //
            .if_value([&](const url& u) { req.uri = u; })
            .if_error([&](auto) { req.uri.path = "/"; });
    }

    auto protocol_raw = view.substr(uri_end + 1);
    // TODO

    while (std::getline(r, line))
    {
        if (line == "") break;

        auto split_idx = line.find(':');
        if (split_idx == std::string::npos) continue; // invalid header

        auto key = line.substr(0, split_idx);
        auto val = trim_string(line.substr(split_idx + 1));
        req.headers[std::string{key}].emplace_back(val);
    }

    // TODO wrap the body up to correctly identify "EOF"
    return result<request, std::error_condition>{req};
}

}
