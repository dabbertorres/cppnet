#pragma once

#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace net
{

class url
{
public:
    using query_values = std::map<std::u8string, std::vector<std::u8string>>;

    struct user_info
    {
        std::u8string username;
        std::u8string password;
    };

    url(const std::u8string&);

    std::u8string build() const noexcept;
    bool          is_valid() const noexcept;

    // decoded/unescaped parts of the URL
    std::u8string scheme;
    user_info     userinfo;
    std::u8string host;
    std::u8string port;
    std::u8string path;
    query_values  query;
    std::u8string fragment;
};

}
