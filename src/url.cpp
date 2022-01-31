#include "url.hpp"

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

parse_state& operator++(parse_state& p)
{
    p = static_cast<parse_state>(static_cast<uint8_t>(p) + 1);
    return p;
}

void url_parse(std::u8string_view s, url& u) noexcept
{
    parse_state state = {};

    // indices into s representing where we're parsing
    size_t             start = 0;
    size_t             end   = 0;
    std::u8string_view raw_query;

    while (end < s.size())
    {
        switch (state)
        {
        case parse_state::scheme:
            if (s.at(end) != ':')
            {
                ++end;
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
            if (s.at(start) == '/' && s.at(end) == '/')
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
                    u.userinfo.username = s.substr(start, split - start);
                    u.userinfo.password = s.substr(split + 1, end - (split + 1));
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
            if (auto idx = s.find_first_of(u8":/?#", start); idx != std::string_view::npos)
            {
                u.host = s.substr(start, idx - start);
                start  = idx + 1;
                end    = start;
                switch (s[idx])
                {
                case ':':
                    state = parse_state::port;
                    break;

                case '/':
                    state = parse_state::path;
                    break;

                case '?':
                    state = parse_state::query;
                    break;

                case '#':
                    state = parse_state::fragment;
                    break;
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
            if (auto idx = s.find_first_of(u8"/?#", start); idx != std::string_view::npos)
            {
                u.port = s.substr(start, idx - start);
                start  = idx + 1;
                end    = start;
                switch (s[idx])
                {
                case '/':
                    state = parse_state::path;
                    break;

                case '?':
                    state = parse_state::query;
                    break;

                case '#':
                    state = parse_state::fragment;
                    break;
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
            if (auto idx = s.find_first_of(u8"?#", start); idx != std::string_view::npos)
            {
                u.path = s.substr(start, idx - start);
                start  = idx + 1;
                end    = start;
                switch (s[idx])
                {
                case '?':
                    state = parse_state::query;
                    break;

                case '#':
                    state = parse_state::fragment;
                    break;
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
            break;

        case parse_state::done:
            goto done;
        }
    }

done:
    // TODO url decode everything

    if (!raw_query.empty())
    {
        size_t start = 0;
        size_t end   = raw_query.find('&');
        while (true)
        {
            auto kv     = raw_query.substr(start, end - start);
            auto eq_idx = kv.find('=');
            auto key    = kv.substr(0, eq_idx);

            if (eq_idx != std::string_view::npos)
            {
                // NOTE: val may be empty
                auto val = kv.substr(eq_idx + 1);
                u.query[std::u8string(key)].emplace_back(val);
            }
            else
            {
                // no value
                u.query.try_emplace(std::u8string(kv));
            }
        }
    }

    if (state != parse_state::done)
    {
        // TODO report the parsing failure in some way
    }
}

url::url(const std::u8string& s)
{
    url_parse(s, *this);
}

std::u8string url::build() const noexcept
{
    // TODO url encode as necessary
}

bool url::is_valid() const noexcept
{
    //
}

}
