#include "url.hpp"

#include <charconv>
#include <sstream>
#include <system_error>

namespace
{

using net::url;
using net::url_parse_state;

url_parse_state& operator++(url_parse_state& p)
{
    p = static_cast<url_parse_state>(static_cast<uint8_t>(p) + 1);
    return p;
}

struct url_parser
{
    std::string_view input;
    url*             out;

    url_parse_state state = url_parse_state::scheme;
    size_t          start = 0;
    size_t          end   = 0;

    void parse_scheme() noexcept;
    void parse_authority() noexcept;
    void parse_userinfo() noexcept;
    void parse_host() noexcept;
    void parse_port() noexcept;
    void parse_path() noexcept;
    void parse_query() noexcept;
    void parse_fragment() noexcept;
};

void url_parser::parse_scheme() noexcept
{
    if (input.at(end) != ':')
    {
        ++end;

        // if we reach the end, there is no scheme, so we need to backtrack
        if (end == input.size())
        {
            end = 0;

            // at this point, the spec is ambiguous, so let's just try and do
            // the "right thing"

            if (input.starts_with("//"))
            {
                // start of authority/host
                state = url_parse_state::authority;
            }
            else if (input.front() == '/')
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
        out->scheme = input.substr(start, end - start);
        start       = end + 1;
        end         = start;
        ++state;
    }
}

void url_parser::parse_authority() noexcept
{
    ++end;
    if (end < input.size() && input.at(start) == '/' && input.at(end) == '/')
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
}

void url_parser::parse_userinfo() noexcept
{
    if (auto idx = input.find('@', start); idx != std::string_view::npos)
    {
        end = idx;

        auto userinfo = input.substr(start, end - start);
        if (auto split = userinfo.find(':'); split != std::string_view::npos)
        {
            // has a password component
            out->userinfo.username = userinfo.substr(0, split);
            out->userinfo.password = userinfo.substr(split + 1, end - (split + 1));
        }
        else
        {
            out->userinfo.username = userinfo;
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
}

void url_parser::parse_host() noexcept
{
    // anything up to either the port, path, query, fragment, or end is the host
    if (auto idx = input.find_first_of(":/?#", start); idx != std::string_view::npos)
    {
        out->host = input.substr(start, idx - start);
        start     = idx + 1;
        end       = start;
        switch (input[idx])
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
        out->host = input.substr(start);
        state     = url_parse_state::done;
        end       = input.size();
    }
}

void url_parser::parse_port() noexcept
{
    // anything up to either the path, query, fragment, or end is the port
    if (auto idx = input.find_first_of("/?#", start); idx != std::string_view::npos)
    {
        out->port = input.substr(start, idx - start);
        start     = idx + 1;
        end       = start;
        switch (input[idx])
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
        out->port = input.substr(start);
        state     = url_parse_state::done;
        end       = input.size();
    }
}

void url_parser::parse_path() noexcept
{
    // anything up to either the query, fragment, or end is the path
    if (auto idx = input.find_first_of("?#", start); idx != std::string_view::npos)
    {
        out->path = input.substr(start, idx - start);
        start     = idx + 1;
        end       = start;
        switch (input[idx])
        {
        case '?': state = url_parse_state::query; break;

        case '#': state = url_parse_state::fragment; break;
        }
    }
    else
    {
        out->path = input.substr(start);
        state     = url_parse_state::done;
        end       = input.size();
    }
}

void url_parser::parse_query() noexcept
{
    std::string_view raw_query;

    // anything up to either the fragment, or end is the path
    if (auto idx = input.find_first_of('#', start); idx != std::string_view::npos)
    {
        raw_query = input.substr(start, idx - start);
        state     = url_parse_state::fragment;
        start     = idx + 1;
        end       = start;
    }
    else
    {
        raw_query = input.substr(start);
        state     = url_parse_state::done;
        end       = input.size();
    }

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
            out->query[key].emplace_back(url::decode(val));
        }
        else
        {
            // no value
            out->query.try_emplace(key);
        }

        if (end == std::string_view::npos) break;
        raw_query = raw_query.substr(end + 1);
    }
}

void url_parser::parse_fragment() noexcept
{
    // everything else!
    out->fragment = input.substr(start);
    state         = url_parse_state::done;
}

url_parse_state url_parse(std::string_view s, url& u) noexcept
{
    url_parser parser{
        .input = s,
        .out   = &u,
    };

    while (parser.end < s.size())
    {
        switch (parser.state)
        {
        case url_parse_state::scheme: parser.parse_scheme(); break;
        case url_parse_state::authority: parser.parse_authority(); break;
        case url_parse_state::userinfo: parser.parse_userinfo(); break;
        case url_parse_state::host: parser.parse_host(); break;
        case url_parse_state::port: parser.parse_port(); break;
        case url_parse_state::path: parser.parse_path(); break;
        case url_parse_state::query: parser.parse_query(); break;
        case url_parse_state::fragment: parser.parse_fragment(); break;
        case url_parse_state::done: parser.end = s.size(); break;
        }
    }

    if (!u.scheme.empty()) u.scheme = url::decode(u.scheme);
    if (!u.userinfo.username.empty()) u.userinfo.username = url::decode(u.userinfo.username);
    if (!u.userinfo.password.empty()) u.userinfo.password = url::decode(u.userinfo.password);
    if (!u.host.empty()) u.host = url::decode(u.host);
    if (!u.port.empty()) u.port = url::decode(u.port);
    if (!u.path.empty()) u.path = url::decode(u.path);
    if (!u.fragment.empty()) u.fragment = url::decode(u.fragment);

    return parser.state;
}

}

namespace net
{

bool url::user_info::empty() const { return username.empty() && password.empty(); }

std::string url::encode(std::string_view str, std::string_view reserved) noexcept
{
    std::ostringstream out;

    size_t idx = 0;
    while ((idx = str.find_first_of(reserved, idx)) != std::string::npos)
    {
        std::array<char, 2> buf{};
        std::to_chars(buf.begin(), buf.end(), str[idx], 16);
        out << '%' << buf[0] << buf[1];
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
        auto hex_val = str.substr(idx + 1, 2);

        std::uint8_t val = 0;

        auto result = std::from_chars(hex_val.begin(), hex_val.end(), val, 16);
        // drop invalid values
        if (result.ec == std::errc())
        {
            out << str.substr(0, idx);
            out << val;
        }

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
    std::ostringstream out;

    // TODO url encode as necessary

    if (!scheme.empty()) out << scheme << "://";
    if (!userinfo.empty())
    {
        if (!userinfo.username.empty()) out << userinfo.username;
        out << ':';
        if (!userinfo.password.empty()) out << userinfo.password;
        out << '@';
    }

    if (!host.empty()) out << host;
    if (!port.empty()) out << ':' << port;
    if (!path.empty()) out << path;

    if (!query.empty())
    {
        char sep = '?';
        for (const auto& param : query)
        {
            for (const auto& val : param.second)
            {
                out << sep;
                sep = '&'; // only the first "separator" is a '?'
                out << param.first << '=' << val;
            }
        }
    }

    if (!fragment.empty()) out << '#' << fragment;

    return out.str();
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
    return lhs.scheme   == rhs.scheme
        && lhs.userinfo == rhs.userinfo
        && lhs.host     == rhs.host
        && lhs.port     == rhs.port
        && lhs.path     == rhs.path
        && lhs.query    == rhs.query
        && lhs.fragment == rhs.fragment;
    // clang-format on
}

}
