#include "dns/config.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <compare>
#include <ctime>
#include <expected>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include "ip_addr.hpp"
#include "ipv4_addr.hpp"
#include "ipv6_addr.hpp"
#include "util/string_util.hpp"

namespace
{

using namespace std::string_view_literals;

// fields splits line into its fields, separated by spaces and tabs (and a '\r', from a file with CRLF line ends).
std::vector<std::string_view> fields(std::string_view line)
{
    constexpr auto space = " \t\r\v\f"sv;

    std::vector<std::string_view> out;
    for (auto start = line.find_first_not_of(space); start != std::string_view::npos;
         start      = line.find_first_not_of(space, start))
    {
        const auto end = line.find_first_of(space, start);
        out.push_back(line.substr(start, end - start));
        if (end == std::string_view::npos) break;
        start = end;
    }
    return out;
}

// lines splits text into its lines, without their '\n's.
std::vector<std::string_view> lines(std::string_view text)
{
    std::vector<std::string_view> out;
    while (!text.empty())
    {
        const auto end = text.find('\n');
        out.push_back(text.substr(0, end));
        if (end == std::string_view::npos) break;
        text.remove_prefix(end + 1);
    }
    return out;
}

// without_comment is line up to a '#', if it has one.
std::string_view without_comment(std::string_view line) noexcept
{
    if (const auto hash = line.find('#'); hash != std::string_view::npos) line.remove_suffix(line.size() - hash);
    return line;
}

// number reads str's leading digits, as Go's dtoi does: none is 0, and too many saturate.
int number(std::string_view str) noexcept
{
    constexpr int big = 0xFF'FFFF;

    int n = 0;
    for (const char c : str)
    {
        if (c < '0' || c > '9') break;
        n = (n * 10) + (c - '0');
        if (n >= big) return big;
    }
    return n;
}

// domain is name without its final dot, lower-cased.
std::string domain(std::string_view name)
{
    if (name.ends_with('.')) name.remove_suffix(1);

    std::string out{name};
    std::ranges::transform(out, out.begin(), [](char c) { return net::util::to_lower_ascii(c); });
    return out;
}

// read_file is path's text, or nothing if it can't be read.
std::string read_file(const std::string& path)
{
    std::ifstream in{path, std::ios::binary};
    if (!in) return {};
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

net::dns::detail::file_stamp stamp_of(const std::string& path) noexcept
{
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) return {};

#ifdef __APPLE__
    const auto& mtime = st.st_mtimespec;
#else
    const auto& mtime = st.st_mtim;
#endif

    return {
        .exists = true,
        .inode  = static_cast<std::uint64_t>(st.st_ino),
        .size   = static_cast<std::int64_t>(st.st_size),
        .mtime_ns =
            (static_cast<std::int64_t>(mtime.tv_sec) * 1'000'000'000) + static_cast<std::int64_t>(mtime.tv_nsec),
    };
}

// read_option reads one of resolv.conf's options into conf.
void read_option(net::dns::config& conf, std::string_view opt)
{
    constexpr int max_ndots = 15;

    if (opt.starts_with("ndots:")) conf.ndots = std::min(number(opt.substr("ndots:"sv.size())), max_ndots);
    else if (opt.starts_with("timeout:"))
        conf.timeout = std::chrono::seconds{std::max(number(opt.substr("timeout:"sv.size())), 1)};
    else if (opt.starts_with("attempts:")) conf.attempts = std::max(number(opt.substr("attempts:"sv.size())), 1);
    else if (opt == "rotate") conf.rotate = true;
    else if (opt == "single-request") conf.single_request = true;
    else if (opt == "single-request-reopen") conf.single_request_reopen = true;
    else if (opt == "use-vc" || opt == "usevc" || opt == "tcp") conf.use_tcp = true;
    else if (opt == "trust-ad") conf.trust_ad = true;
    else if (opt != "edns0") conf.unknown_option = true; // EDNS(0) is always on
}

// read_resolv_line reads one of resolv.conf's lines (its fields, at least one) into conf, setting searched if it's a
// search or domain line.
void read_resolv_line(net::dns::config& conf, bool& searched, std::span<const std::string_view> f)
{
    const auto keyword = f.front();
    const auto args    = f.subspan(1);

    if (keyword == "nameserver")
    {
        if (args.empty() || conf.nameservers.size() >= net::dns::max_nameservers) return;
        // NOTE: an IPv6 address with a zone (fe80::1%eth0) doesn't parse, as ip_addr has none.
        if (auto addr = net::ip_addr::parse(args.front()); addr.has_value())
            conf.nameservers.push_back({.addr = *addr});
    }
    else if (keyword == "domain")
    {
        if (args.empty()) return;
        searched = true;
        conf.search.clear();
        if (auto name = domain(args.front()); !name.empty()) conf.search.push_back(std::move(name));
    }
    else if (keyword == "search")
    {
        searched = true;
        conf.search.clear();
        for (const auto arg : args)
        {
            if (auto name = domain(arg); !name.empty()) conf.search.push_back(std::move(name));
        }
    }
    else if (keyword == "options")
    {
        for (const auto opt : args) read_option(conf, opt);
    }
    else
    {
        conf.unknown_option = true;
    }
}

// read_nss_sources reads the sources of nsswitch.conf's hosts line, after its colon (and without its comment).
std::vector<net::dns::nss_source> read_nss_sources(std::string_view line)
{
    constexpr auto space = " \t\r"sv;

    std::vector<net::dns::nss_source> sources;
    for (auto i = line.find_first_not_of(space); i != std::string_view::npos; i = line.find_first_not_of(space, i))
    {
        if (line[i] == '[')
        {
            // an action, of the source before it: up to its ']' (or the end of the line).
            const auto end    = line.find(']', i);
            const auto action = line.substr(i, end == std::string_view::npos ? end : end + 1 - i);
            if (!sources.empty()) sources.back().action.append(action);
            i += action.size();
        }
        else
        {
            const auto end  = line.find_first_of(" \t\r[", i);
            const auto name = line.substr(i, end == std::string_view::npos ? end : end - i);
            sources.push_back({.name = std::string{name}, .action = {}});
            i += name.size();
        }
    }
    return sources;
}

}

namespace net::dns
{

config parse_resolv_conf(std::string_view text, std::string_view hostname)
{
    config conf;
    bool   searched = false; // whether there was a search or domain line

    for (const auto line : lines(text))
    {
        if (line.starts_with('#') || line.starts_with(';')) continue;

        const auto f = fields(line);
        if (!f.empty()) read_resolv_line(conf, searched, f);
    }

    if (conf.nameservers.empty())
    {
        conf.nameservers = {
            {.addr = ipv4_addr::loopback()},
            {.addr = ipv6_addr::loopback()},
        };
    }

    // without a search list, the host name's domain, if it has one (not counting a dot that starts it).
    if (const auto dot = hostname.find('.', 1); !searched && dot != std::string_view::npos)
    {
        if (auto name = domain(hostname.substr(dot + 1)); !name.empty()) conf.search.push_back(std::move(name));
    }

    return conf;
}

hosts_table parse_hosts(std::string_view text)
{
    hosts_table table;

    for (const auto line : lines(text))
    {
        const auto f = fields(without_comment(line));
        if (f.size() < 2) continue;

        // NOTE: an IPv6 address with a zone doesn't parse, as ip_addr has none.
        const auto addr = ip_addr::parse(f.front());
        if (!addr.has_value()) continue;

        for (const auto name : std::span{f}.subspan(1))
        {
            if (auto key = domain(name); !key.empty()) table[std::move(key)].push_back(*addr);
        }
    }

    return table;
}

std::vector<nss_source> parse_nsswitch_hosts(std::string_view text)
{
    std::vector<nss_source> sources;

    for (const auto line : lines(text))
    {
        const auto content = without_comment(line);
        const auto colon   = content.find(':');
        if (colon == std::string_view::npos || util::trim_string(content.substr(0, colon)) != "hosts") continue;

        // the last hosts line wins, as glibc reads it.
        sources = read_nss_sources(content.substr(colon + 1));
    }

    return sources;
}

lookup_order order_of(const std::vector<nss_source>& sources) noexcept
{
    const auto files = std::ranges::find(sources, "files", &nss_source::name);
    const auto dns   = std::ranges::find(sources, "dns", &nss_source::name);

    const bool has_files = files != sources.end();
    const bool has_dns   = dns != sources.end();

    if (has_files && has_dns) return files < dns ? lookup_order::files_dns : lookup_order::dns_files;
    if (has_files) return lookup_order::files;
    if (has_dns) return lookup_order::dns;
    return lookup_order::files_dns;
}

namespace detail
{

    config read_config(const config_files& files)
    {
        auto conf       = parse_resolv_conf(read_file(files.resolv_conf), host_name());
        conf.hosts      = parse_hosts(read_file(files.hosts));
        conf.nss_hosts  = parse_nsswitch_hosts(read_file(files.nsswitch_conf));
        conf.order      = order_of(conf.nss_hosts);
        conf.mdns_allow = stamp_of(files.mdns_allow).exists;
        return conf;
    }

    std::string host_name()
    {
        std::array<char, 256> name{};
        if (::gethostname(name.data(), name.size() - 1) != 0) return {};
        return name.data();
    }

    config_cache::config_cache(std::optional<config> given, config_files files)
        : files{std::move(files)}
        , given{given.has_value()}
    {
        if (given.has_value()) current = std::make_shared<const config>(std::move(*given));
    }

    std::shared_ptr<const config> config_cache::get()
    {
        const std::scoped_lock lock{mu};
        if (given) return current;

        const auto now = std::chrono::steady_clock::now();
        if (current != nullptr && now < next_check) return current;
        next_check = now + files.check_interval;

        // NOTE: stamped before reading, so a change while it's read is seen at the next check.
        const auto now_stamps = stamps();
        if (current != nullptr && now_stamps == read_stamps) return current;

        current     = std::make_shared<const config>(read_config(files));
        read_stamps = now_stamps;
        return current;
    }

    std::array<file_stamp, 4> config_cache::stamps() const
    {
        return {
            stamp_of(files.resolv_conf),
            stamp_of(files.hosts),
            stamp_of(files.nsswitch_conf),
            stamp_of(files.mdns_allow),
        };
    }

}

}
