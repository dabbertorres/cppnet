#pragma once

#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "util.hpp"

namespace net
{

enum class parse_state : uint8_t
{
    scheme,
    authority,
    userinfo,
    host,
    port,
    path,
    query,
    fragment,
    done,
};

constexpr std::string_view parse_state_string(parse_state s) noexcept
{
    using enum parse_state;

    switch (s)
    {
    case scheme:
        return "scheme";
    case authority:
        return "authority";
    case userinfo:
        return "userinfo";
    case host:
        return "host";
    case port:
        return "port";
    case path:
        return "path";
    case query:
        return "query";
    case fragment:
        return "fragment";
    case done:
        return "done";
    }
}

struct url_parse_failure
{
    parse_state failed_at;
};

class url
{
public:
    struct user_info
    {
        std::string username;
        std::string password;
    };

    using query_values = std::unordered_map<std::string, std::vector<std::string>>;

    static constexpr auto reserved_characters = "!#$%&'()*+,/:;=?@[]";

    static std::string encode(std::string_view str,
                              std::string_view reserved = reserved_characters) noexcept;
    static std::string decode(std::string_view str) noexcept;

    using parse_result = util::result<url, url_parse_failure>;

    static parse_result parse(const char*) noexcept;
    static parse_result parse(const std::string&) noexcept;
    static parse_result parse(std::string_view) noexcept;

    std::string build() const noexcept;
    /* bool        is_valid() const noexcept; */

    // decoded/unescaped parts of the URL
    std::string  scheme;
    user_info    userinfo;
    std::string  host;
    std::string  port;
    std::string  path;
    query_values query;
    std::string  fragment;
};

}
