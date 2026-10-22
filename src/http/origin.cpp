#include "http/origin.hpp"

#include <algorithm>
#include <expected>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "ip_addr.hpp"
#include "util/string_util.hpp"

namespace net::http::detail
{

namespace
{

    // dns_name is whether host is a DNS name, as a certificate's are (without wildcards): labels of letters, digits and
    // hyphens (not at either end), each 1 to 63 of them, and 253 in all.
    bool dns_name(std::string_view host)
    {
        if (host.empty() || host.size() > 253) return false;

        for (const auto label : util::split_string(host, '.'))
        {
            if (label.empty() || label.size() > 63 || label.front() == '-' || label.back() == '-') return false;

            const auto ldh = [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; };
            if (!std::ranges::all_of(label, ldh)) return false;
        }
        return true;
    }

    // host_of reads the host at the start of rest (lowercase), removing it: an IPv6 address in brackets, or up to the
    // port; nothing if it isn't one.
    std::expected<https_origin, std::error_code> host_of(std::string_view& rest)
    {
        const auto invalid = std::unexpected(std::make_error_code(std::errc::invalid_argument));

        if (rest.starts_with('['))
        {
            const auto close = rest.find(']');
            if (close == std::string_view::npos) return invalid;

            const auto addr = ip_addr::parse(rest.substr(1, close - 1));
            if (!addr.has_value() || !addr->is_ipv6()) return invalid;
            rest.remove_prefix(close + 1);

            auto host = addr->to_string();
            return https_origin{.serialized = "[" + host + "]", .host = std::move(host)};
        }

        const auto name = rest.substr(0, rest.find(':'));
        rest.remove_prefix(name.size());

        std::string host;
        if (const auto addr = ip_addr::parse(name); addr.has_value() && addr->is_ipv4()) host = addr->to_string();
        else if (dns_name(name)) host = name;
        else return invalid;
        return https_origin{.serialized = host, .host = host};
    }

    // port_of reads what follows an origin's host: nothing, or ':' and a port from 1 to 65535. It returns what to write
    // of it (nothing for none, or the default, 443).
    std::expected<std::string, std::error_code> port_of(std::string_view rest)
    {
        const auto invalid = std::unexpected(std::make_error_code(std::errc::invalid_argument));

        if (rest.empty()) return std::string{};
        if (!rest.starts_with(':')) return invalid;
        rest.remove_prefix(1);

        const auto digit = [](char c) { return c >= '0' && c <= '9'; };
        if (rest.empty() || rest.size() > 5 || !std::ranges::all_of(rest, digit)) return invalid;

        unsigned port = 0;
        for (const auto c : rest) port = (port * 10) + static_cast<unsigned>(c - '0');
        if (port == 0 || port > 65'535) return invalid;
        return port == 443 ? std::string{} : ":" + std::to_string(port);
    }

}

std::expected<https_origin, std::error_code> parse_origin(std::string_view text)
{
    constexpr std::string_view scheme = "https://";
    if (text.size() < scheme.size() || !util::equal_ignore_case(text.substr(0, scheme.size()), scheme))
        return std::unexpected(std::make_error_code(std::errc::invalid_argument));
    text.remove_prefix(scheme.size());

    std::string lower{text};
    for (auto& c : lower) c = util::to_lower_ascii(c);
    std::string_view rest = lower;

    auto host = host_of(rest);
    if (!host.has_value()) return std::unexpected(host.error());
    const auto port = port_of(rest);
    if (!port.has_value()) return std::unexpected(port.error());

    host->serialized = std::string{scheme} + host->serialized + *port;
    return host;
}

std::string origin_text(std::string_view host, std::string_view port)
{
    std::string out{"https://"};

    // (an IPv6 address always has a colon, which a name never has.)
    if (host.contains(':')) out.append("[").append(host).append("]");
    else out.append(host);

    if (!port.empty()) out.append(":").append(port);
    return out;
}

}
