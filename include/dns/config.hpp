#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ip_addr.hpp"

// The native resolver's configuration (see resolver::backend::native): what /etc/resolv.conf, /etc/hosts and
// /etc/nsswitch.conf say, as Go's resolver reads them, or what a program gives it instead.
namespace net::dns
{

// nameserver is a DNS server to ask: its address and port.
struct nameserver
{
    ip_addr       addr;
    std::uint16_t port = 53;

    bool operator==(const nameserver&) const = default;
};

// lookup_order is where names are looked up, and in which order: nsswitch.conf's hosts line, as far as its files
// (the hosts table) and dns go.
enum class lookup_order : std::uint8_t
{
    files_dns, // the hosts table, then DNS (the default)
    dns_files, // DNS, then the hosts table
    files,     // only the hosts table
    dns,       // only DNS
};

// hosts_table maps names (lower-cased, without a final dot) to their addresses, in the order the hosts file lists
// them.
using hosts_table = std::map<std::string, std::vector<ip_addr>, std::less<>>;

// nss_source is a source of nsswitch.conf's hosts line: its name, and the action that follows it (e.g.
// "[NOTFOUND=return]", as written; empty if none).
struct nss_source
{
    std::string name{};
    std::string action{};

    bool operator==(const nss_source&) const = default;
};

// config is what the native resolver works from.
struct config
{
    // the servers to ask, in order (as resolv.conf's nameserver lines). None: no DNS (only the hosts table).
    std::vector<nameserver> nameservers{};

    // the domains a name with fewer than ndots dots is tried under (before it's tried as it is), without final dots
    // (resolv.conf's search or domain).
    std::vector<std::string> search{};
    int                      ndots = 1;

    // how long to wait for a server's answer, and how many rounds over the servers to make.
    std::chrono::milliseconds timeout{std::chrono::seconds{5}};
    int                       attempts = 2;

    bool rotate                = false; // each lookup starts at the next server, rather than the first
    bool single_request        = false; // A and AAAA asked in turn, rather than both at once
    bool single_request_reopen = false; // ... each on a socket of its own
    bool use_tcp               = false; // TCP only (use-vc)
    bool trust_ad              = false; // queries set the AD bit

    // whether resolv.conf has something the native resolver doesn't do (an unknown option or keyword).
    bool unknown_option = false;

    hosts_table  hosts{};
    lookup_order order = lookup_order::files_dns;

    // For resolver::backend::automatic's choice (as order is what the native back end makes of them): nsswitch.conf's
    // hosts line (none: it has none, or there's no nsswitch.conf), and whether /etc/mdns.allow is there (which may let
    // an mdns source answer names besides .local ones).
    std::vector<nss_source> nss_hosts{};
    bool                    mdns_allow = false;

    bool operator==(const config&) const = default;
};

// max_nameservers is how many nameserver lines resolv.conf may have (glibc's MAXNS): the rest are ignored.
inline constexpr std::size_t max_nameservers = 3;

// parse_resolv_conf reads resolv.conf's text, as Go's resolver does, into a config's resolv.conf fields (the rest are
// left as they default): nameserver lines (IP addresses, at most max_nameservers; an IPv6 address with a zone is
// skipped, as ip_addr has none), search and domain (the last one wins; "." is dropped), and options ndots:n (0 to 15),
// timeout:n (seconds, at least 1), attempts:n (at least 1), rotate, single-request, single-request-reopen, use-vc (or
// usevc, or tcp, as Go reads them), trust-ad, and edns0 (always on: ignored). A number is read from its leading digits
// (none is 0, then raised to the minimum). Lines starting with '#' or ';' are comments; anything else (another option,
// e.g. no-reload, or keyword) sets unknown_option. Without nameservers, it's 127.0.0.1 and ::1 (Go's and glibc's
// default); without search or domain lines, the domain of hostname (what follows its first dot), if it has one.
[[nodiscard]] config parse_resolv_conf(std::string_view text, std::string_view hostname = {});

// parse_hosts reads a hosts file's text: lines of an IP address and the names it has, '#' starting a comment. Names are
// lower-cased, without a final dot, and a name on several lines has each line's address, in order. Lines with an
// address that isn't one (or has a zone), or without names, are skipped.
[[nodiscard]] hosts_table parse_hosts(std::string_view text);

// parse_nsswitch_hosts reads nsswitch.conf's text, returning its hosts line's sources (none if it has none). '#'
// starts a comment.
[[nodiscard]] std::vector<nss_source> parse_nsswitch_hosts(std::string_view text);

// order_of is the order sources give files and dns (Go's): as they come, if both are there; only one, if only it is.
// Without either (e.g. no hosts line), it's files then DNS.
[[nodiscard]] lookup_order order_of(const std::vector<nss_source>& sources) noexcept;

namespace detail
{

    // config_files are the files the native resolver's configuration is read from, if it isn't given one, and how
    // often they're checked for changes. (Internal: tests point them elsewhere.)
    struct config_files
    {
        std::string                         resolv_conf   = "/etc/resolv.conf";
        std::string                         hosts         = "/etc/hosts";
        std::string                         nsswitch_conf = "/etc/nsswitch.conf";
        std::string                         mdns_allow    = "/etc/mdns.allow"; // only whether it's there
        std::chrono::steady_clock::duration check_interval{std::chrono::seconds{5}};
    };

    // read_config reads files into a config (a file that can't be read counts as empty, so resolv.conf's defaults
    // apply), with the machine's host name for resolv.conf's default search domain.
    [[nodiscard]] config read_config(const config_files& files);

    // host_name is the machine's name (gethostname()'s), or empty if it can't be had.
    [[nodiscard]] std::string host_name();

    // file_stamp is what tells whether a file changed: whether it's there, and its inode, size and modification time.
    struct file_stamp
    {
        bool          exists   = false;
        std::uint64_t inode    = 0;
        std::int64_t  size     = 0;
        std::int64_t  mtime_ns = 0;

        bool operator==(const file_stamp&) const = default;
    };

    // config_cache holds the native resolver's configuration: the one it was given, or else the one read from its
    // files, read when it's first needed, and again if they've changed (checked at most every check_interval, as Go
    // does). Thread-safe.
    class config_cache
    {
    public:
        config_cache(std::optional<config> given, config_files files);

        // get is the configuration now: those that hold it keep theirs, when it's read again.
        [[nodiscard]] std::shared_ptr<const config> get();

    private:
        [[nodiscard]] std::array<file_stamp, 4> stamps() const;

        std::mutex                            mu;
        std::shared_ptr<const config>         current;
        config_files                          files;
        bool                                  given = false;
        std::chrono::steady_clock::time_point next_check{};
        std::array<file_stamp, 4>             read_stamps{};
    };

}

}
