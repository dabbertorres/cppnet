#include "url.hpp"

#include <sstream>

namespace
{

using net::url;
using net::url_parse_state;

// assumes c is a valid hex character
uint8_t parse_hex(char c) noexcept
{
    c = static_cast<char>(std::toupper(c));

    if ('0' <= c && c <= '9') return static_cast<uint8_t>(c - '0');
    if ('A' <= c && c <= 'F') return static_cast<uint8_t>(c - 'A' + 0xa);

    // TODO what do?
}

// assumes s.size() == 2
uint8_t parse_hex(std::string_view s) noexcept { return static_cast<uint8_t>(parse_hex(s[0]) << 4) | parse_hex(s[1]); }

std::string to_hex(char c) noexcept
{
    constexpr auto nibble_to_hex = [](char v) -> char { return v < 0xa ? v + '0' : v - 0xa + 'A'; };

    char top = (c & 0xf0) >> 4;
    char bot = c & 0x0f;

    return {nibble_to_hex(top), nibble_to_hex(bot)};
}

url_parse_state& operator++(url_parse_state& p)
{
    p = static_cast<url_parse_state>(static_cast<uint8_t>(p) + 1);
    return p;
}

url_parse_state url_parse(std::string_view s, url& u) noexcept
{
    auto state = url_parse_state::scheme;

    // indices into s representing where we're parsing
    size_t           start = 0;
    size_t           end   = 0;
    std::string_view raw_query;

    while (end < s.size())
    {
        switch (state)
        {
        case url_parse_state::scheme:
            if (s.at(end) != ':')
            {
                ++end;

                // if we reach the end, there is no scheme, so we need to backtrack
                if (end == s.size())
                {
                    end = 0;

                    // at this point, the spec is ambiguous, so let's just try and do
                    // the "right thing"

                    if (s.starts_with("//"))
                    {
                        // start of authority/host
                        state = url_parse_state::authority;
                    }
                    else if (s.front() == '/')
                    {
                        // probably an absolute path
                        state = url_parse_state::path;
                    }
                    else
                    {
                        // probably the host
                        state = url_parse_state::host;
                    }
                }
            }
            else
            {
                u.scheme = s.substr(start, end - start);
                start    = end + 1;
                end      = start;
                ++state;
            }
            break;

        case url_parse_state::authority:
            ++end;
            if (end < s.size() && s.at(start) == '/' && s.at(end) == '/')
            {
                state = url_parse_state::userinfo;
                start = end + 1;
                end   = start;
            }
            else
            {
                state = url_parse_state::path;
                // backtrack, since this was actually part of the path
                end = start;
            }
            break;

        case url_parse_state::userinfo:
            if (auto idx = s.find('@', start); idx != std::string_view::npos)
            {
                end = idx;

                auto userinfo = s.substr(start, end - start);
                if (auto split = userinfo.find(':'); split != std::string_view::npos)
                {
                    // has a password component
                    u.userinfo.username = userinfo.substr(0, split);
                    u.userinfo.password = userinfo.substr(split + 1, end - (split + 1));
                }
                else
                {
                    u.userinfo.username = userinfo;
                }

                state = url_parse_state::host;
                start = end + 1;
                end   = start;
            }
            else
            {
                // no userinfo
                state = url_parse_state::host;
            }
            break;

        case url_parse_state::host:
            // anything up to either the port, path, query, fragment, or end is the host
            if (auto idx = s.find_first_of(":/?#", start); idx != std::string_view::npos)
            {
                u.host = s.substr(start, idx - start);
                start  = idx + 1;
                end    = start;
                switch (s[idx])
                {
                case ':': state = url_parse_state::port; break;

                case '/':
                    state = url_parse_state::path;
                    // include in the path
                    start = idx;
                    end   = start;
                    break;

                case '?': state = url_parse_state::query; break;

                case '#': state = url_parse_state::fragment; break;
                }
            }
            else
            {
                u.host = s.substr(start);
                state  = url_parse_state::done;
                end    = s.size();
            }
            break;

        case url_parse_state::port:
            // anything up to either the path, query, fragment, or end is the port
            if (auto idx = s.find_first_of("/?#", start); idx != std::string_view::npos)
            {
                u.port = s.substr(start, idx - start);
                start  = idx + 1;
                end    = start;
                switch (s[idx])
                {
                case '/':
                    state = url_parse_state::path;
                    // include in the path
                    start = idx;
                    end   = start;
                    break;

                case '?': state = url_parse_state::query; break;

                case '#': state = url_parse_state::fragment; break;
                }
            }
            else
            {
                u.port = s.substr(start);
                state  = url_parse_state::done;
                end    = s.size();
            }
            break;

        case url_parse_state::path:
            // anything up to either the query, fragment, or end is the path
            if (auto idx = s.find_first_of("?#", start); idx != std::string_view::npos)
            {
                u.path = s.substr(start, idx - start);
                start  = idx + 1;
                end    = start;
                switch (s[idx])
                {
                case '?': state = url_parse_state::query; break;

                case '#': state = url_parse_state::fragment; break;
                }
            }
            else
            {
                u.path = s.substr(start);
                state  = url_parse_state::done;
                end    = s.size();
            }
            break;

        case url_parse_state::query:
            // anything up to either the fragment, or end is the path
            if (auto idx = s.find_first_of('#', start); idx != std::string_view::npos)
            {
                raw_query = s.substr(start, idx - start);
                state     = url_parse_state::fragment;
                start     = idx + 1;
                end       = start;
            }
            else
            {
                raw_query = s.substr(start);
                state     = url_parse_state::done;
                end       = s.size();
            }
            break;

        case url_parse_state::fragment:
            // everything else!
            u.fragment = s.substr(start);
            state      = url_parse_state::done;
            break;

        case url_parse_state::done: end = s.size(); break;
        }
    }

    if (!u.scheme.empty()) u.scheme = url::decode(u.scheme);
    if (!u.userinfo.username.empty()) u.userinfo.username = url::decode(u.userinfo.username);
    if (!u.userinfo.password.empty()) u.userinfo.password = url::decode(u.userinfo.password);
    if (!u.host.empty()) u.host = url::decode(u.host);
    if (!u.port.empty()) u.port = url::decode(u.port);
    if (!u.path.empty()) u.path = url::decode(u.path);
    if (!u.fragment.empty()) u.fragment = url::decode(u.fragment);

    while (!raw_query.empty())
    {
        end = raw_query.find('&');

        auto kv     = raw_query.substr(0, end);
        auto eq_idx = kv.find('=');
        auto key    = url::decode(kv.substr(0, eq_idx));

        if (eq_idx != std::string_view::npos)
        {
            // NOTE: val may be empty
            auto val = kv.substr(eq_idx + 1);
            u.query[key].emplace_back(url::decode(val));
        }
        else
        {
            // no value
            u.query.try_emplace(key);
        }

        if (end == std::string_view::npos) break;
        raw_query = raw_query.substr(end + 1);
    }

    return state;
}

}

namespace net
{

std::string url::encode(std::string_view str, std::string_view reserved) noexcept
{
    std::basic_ostringstream<char> out;

    size_t idx = 0;
    while ((idx = str.find_first_of(reserved, idx)) != std::string::npos)
    {
        out << str.substr(0, idx);
        out << '%' << to_hex(str[idx]);
        str = str.substr(idx + 1);
    }

    out << str;

    return out.str();
}

std::string url::decode(std::string_view str) noexcept
{
    std::basic_ostringstream<char> out;

    size_t idx = 0;
    while ((idx = str.find('%')) != std::string::npos)
    {
        out << str.substr(0, idx);

        auto hex_val = str.substr(idx + 1, 2);
        out << parse_hex(hex_val);

        str = str.substr(idx + 3);
    }

    out << str;

    return out.str();
}

url::parse_result url::parse(const char* s) noexcept { return parse(std::string_view(s)); }

url::parse_result url::parse(const std::string& s) noexcept { return parse(std::string_view{s}); }

url::parse_result url::parse(std::string_view s) noexcept
{
    url  u;
    auto end_state = url_parse(s, u);
    if (end_state != url_parse_state::done) return {url_parse_failure{end_state}};
    return {u};
}

std::string url::build() const noexcept
{
    // TODO url encode as necessary
}

/* bool url::is_valid() const noexcept */
/* { */
/*     // TODO */
/* } */

bool operator==(const url::user_info& lhs, const url::user_info& rhs) noexcept
{
    return lhs.username == rhs.username && lhs.password == rhs.password;
}

bool operator==(const url& lhs, const url& rhs) noexcept
{
    // clang-format off
    return lhs.scheme == rhs.scheme
            && lhs.userinfo == rhs.userinfo
            && lhs.host == rhs.host
            && lhs.port == rhs.port
            && lhs.path == rhs.path
            && lhs.query == rhs.query
            && lhs.fragment == rhs.fragment;
    // clang-format on
}

}
