#include "url.hpp"

#include <sstream>

namespace
{

using namespace net;

// assumes c is a valid hex character
uint8_t parse_hex(char c) noexcept
{
    c = std::toupper(c);

    if ('0' <= c && c <= '9')
        return c - '0';
    else if ('A' <= c && c <= 'F')
        return c - 'A' + 0xa;
    else
        ; // TODO what do?
}

// assumes s.size() == 2
std::string_view::value_type parse_hex(std::string_view s) noexcept { return (parse_hex(s[0]) << 4) | parse_hex(s[1]); }

std::string to_hex(uint8_t b) noexcept
{
    constexpr auto nibble_to_hex = [](uint8_t v) -> char { return v < 0xa ? v + '0' : v - 0xa + 'A'; };

    uint8_t top = (b & 0xf0) >> 4;
    uint8_t bot = b & 0x0f;

    return {nibble_to_hex(top), nibble_to_hex(bot)};
}

parse_state& operator++(parse_state& p)
{
    p = static_cast<parse_state>(static_cast<uint8_t>(p) + 1);
    return p;
}

parse_state url_parse(std::string_view s, url& u) noexcept
{
    auto state = parse_state::scheme;

    // indices into s representing where we're parsing
    size_t           start = 0;
    size_t           end   = 0;
    std::string_view raw_query;

    while (end < s.size())
    {
        switch (state)
        {
        case parse_state::scheme:
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
                        state = parse_state::authority;
                    }
                    else if (s.front() == '/')
                    {
                        // probably an absolute path
                        state = parse_state::path;
                    }
                    else
                    {
                        // probably the host
                        state = parse_state::host;
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

        case parse_state::authority:
            ++end;
            if (end < s.size() && s.at(start) == '/' && s.at(end) == '/')
            {
                state = parse_state::userinfo;
                start = end + 1;
                end   = start;
            }
            else
            {
                state = parse_state::path;
                // backtrack, since this was actually part of the path
                end = start;
            }
            break;

        case parse_state::userinfo:
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

                state = parse_state::host;
                start = end + 1;
                end   = start;
            }
            else
            {
                // no userinfo
                state = parse_state::host;
            }
            break;

        case parse_state::host:
            // anything up to either the port, path, query, fragment, or end is the host
            if (auto idx = s.find_first_of(":/?#", start); idx != std::string_view::npos)
            {
                u.host = s.substr(start, idx - start);
                start  = idx + 1;
                end    = start;
                switch (s[idx])
                {
                case ':': state = parse_state::port; break;

                case '/':
                    state = parse_state::path;
                    // include in the path
                    start = idx;
                    end   = start;
                    break;

                case '?': state = parse_state::query; break;

                case '#': state = parse_state::fragment; break;
                }
            }
            else
            {
                u.host = s.substr(start);
                state  = parse_state::done;
                end    = s.size();
            }
            break;

        case parse_state::port:
            // anything up to either the path, query, fragment, or end is the port
            if (auto idx = s.find_first_of("/?#", start); idx != std::string_view::npos)
            {
                u.port = s.substr(start, idx - start);
                start  = idx + 1;
                end    = start;
                switch (s[idx])
                {
                case '/':
                    state = parse_state::path;
                    // include in the path
                    start = idx;
                    end   = start;
                    break;

                case '?': state = parse_state::query; break;

                case '#': state = parse_state::fragment; break;
                }
            }
            else
            {
                u.port = s.substr(start);
                state  = parse_state::done;
                end    = s.size();
            }
            break;

        case parse_state::path:
            // anything up to either the query, fragment, or end is the path
            if (auto idx = s.find_first_of("?#", start); idx != std::string_view::npos)
            {
                u.path = s.substr(start, idx - start);
                start  = idx + 1;
                end    = start;
                switch (s[idx])
                {
                case '?': state = parse_state::query; break;

                case '#': state = parse_state::fragment; break;
                }
            }
            else
            {
                u.path = s.substr(start);
                state  = parse_state::done;
                end    = s.size();
            }
            break;

        case parse_state::query:
            // anything up to either the fragment, or end is the path
            if (auto idx = s.find_first_of('#', start); idx != std::string_view::npos)
            {
                raw_query = s.substr(start, idx - start);
                state     = parse_state::fragment;
                start     = idx + 1;
                end       = start;
            }
            else
            {
                raw_query = s.substr(start);
                state     = parse_state::done;
                end       = s.size();
            }
            break;

        case parse_state::fragment:
            // everything else!
            u.fragment = s.substr(start);
            state      = parse_state::done;
            break;

        case parse_state::done: goto done;
        }
    }

done:
    if (!u.scheme.empty()) u.scheme = url::decode(u.scheme);
    if (!u.userinfo.username.empty()) u.userinfo.username = url::decode(u.userinfo.username);
    if (!u.userinfo.password.empty()) u.userinfo.password = url::decode(u.userinfo.password);
    if (!u.host.empty()) u.host = url::decode(u.host);
    if (!u.port.empty()) u.port = url::decode(u.port);
    if (!u.path.empty()) u.path = url::decode(u.path);
    if (!u.fragment.empty()) u.fragment = url::decode(u.fragment);

    while (!raw_query.empty())
    {
        size_t end = raw_query.find('&');

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

std::string url::encode(std::string_view str, std::string_view reserved_characters) noexcept
{
    std::ostringstream out;

    size_t idx = 0;
    while ((idx = str.find_first_of(reserved_characters, idx)) != std::string::npos)
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
    std::ostringstream out;

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

url::parse_result url::parse(const char* s) noexcept { return parse(std::string_view{s}); }

url::parse_result url::parse(const std::string& s) noexcept { return parse(std::string_view{s}); }

url::parse_result url::parse(std::string_view s) noexcept
{
    url  u;
    auto end_state = url_parse(s, u);
    if (end_state != parse_state::done)
        return {url_parse_failure{end_state}};
    else
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

}
