#include "dns/automatic.hpp"

#include <cstdint>
#include <cstdlib>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "config.hpp" // IWYU pragma: keep (for the platform macros)
#include "dns/resolver.hpp"
#include "util/string_util.hpp"

namespace
{

using namespace std::string_view_literals;

using backend = net::dns::resolver::backend;

// this_os_native is whether the native back end reads this OS's configuration: Linux's and the BSDs', but OpenBSD's,
// which orders the hosts table and DNS with resolv.conf's lookup keyword (which the native back end counts unknown),
// not nsswitch.conf.
constexpr bool this_os_native =
#if defined(NET_IS_LINUX) || (defined(NET_IS_BSD) && !defined(__OpenBSD__))
    true;
#else
    false;
#endif

std::optional<std::string> env(const net::dns::detail::system_facts& facts, const char* name)
{
    if (facts.getenv) return facts.getenv(name);

    // NOLINTNEXTLINE(concurrency-mt-unsafe): read once, when the resolver is made.
    const char* value = std::getenv(name);
    if (value == nullptr) return std::nullopt;
    return std::string{value};
}

bool iequals(std::string_view lhs, std::string_view rhs) noexcept { return net::util::equal_ignore_case(lhs, rhs); }

bool ends_with_ignore_case(std::string_view str, std::string_view suffix) noexcept
{
    if (str.size() < suffix.size()) return false;
    str.remove_prefix(str.size() - suffix.size());
    return iequals(str, suffix);
}

// is_localhost is whether name is one of the localhost names myhostname answers (Go's isLocalhost).
bool is_localhost(std::string_view name) noexcept
{
    return iequals(name, "localhost") || iequals(name, "localhost.localdomain")
        || ends_with_ignore_case(name, ".localhost") || ends_with_ignore_case(name, ".localhost.localdomain");
}

// fields splits text into its fields, separated by whitespace.
std::vector<std::string_view> fields(std::string_view text)
{
    constexpr auto space = " \t\r\n\v\f"sv;

    std::vector<std::string_view> out;
    for (auto start = text.find_first_not_of(space); start != std::string_view::npos;
         start      = text.find_first_not_of(space, start))
    {
        const auto end = text.find_first_of(space, start);
        out.push_back(text.substr(start, end - start));
        if (end == std::string_view::npos) break;
        start = end;
    }
    return out;
}

// action_kind is what a source's action (as parse_nsswitch_hosts() keeps it) is to the native back end.
enum class action_kind : std::uint8_t
{
    standard,    // as if there were none: each criterion its status's default
    nonstandard, // something else, which the native back end doesn't do
    unreadable,  // one Go can't read (and so leaves nsswitch.conf to the C library)
};

// criterion_kind is what one of an action's criteria ([!]STATUS=ACTION, any case) is, on the last source or not.
action_kind criterion_kind(std::string_view criterion, bool last_source)
{
    // NOTE: Go's reading: at least 3 characters (after a '!'), and an '='.
    const bool negated = criterion.starts_with('!');
    if (negated) criterion.remove_prefix(1);

    const auto eq = criterion.find('=');
    if (criterion.size() < 3 || eq == std::string_view::npos) return action_kind::unreadable;
    if (negated) return action_kind::nonstandard;

    const auto status = criterion.substr(0, eq);
    const auto action = criterion.substr(eq + 1);

    std::string_view standard;
    if (iequals(status, "success")) standard = "return";
    else if (iequals(status, "notfound") || iequals(status, "unavail") || iequals(status, "tryagain"))
        standard = "continue";
    else return action_kind::nonstandard;

    // NOTE: on the last source, returning is what continuing comes to, as nothing follows. (Go allows "return" on a
    //       source's last criterion, wherever the source is, though e.g. "files [NOTFOUND=return] dns" then never
    //       asks DNS for a name the hosts table hasn't, as the native back end would.)
    if (iequals(action, standard) || (last_source && iequals(action, "return"))) return action_kind::standard;
    return action_kind::nonstandard;
}

// action_kind_of is what a source's action is: none, or one bracketed list of criteria.
action_kind action_kind_of(std::string_view action, bool last_source)
{
    if (action.empty()) return action_kind::standard;

    // NOTE: parse_nsswitch_hosts() appends the source's bracketed lists to its action, so a second shows as "][", and
    //       an unclosed one doesn't end in ']'. Go reads a second as a source of its own (which it doesn't know).
    if (!action.starts_with('[') || !action.ends_with(']')) return action_kind::unreadable;
    action = action.substr(1, action.size() - 2);
    if (action.find_first_of("[]") != std::string_view::npos) return action_kind::nonstandard;

    auto kind = action_kind::standard;
    for (const auto criterion : fields(action))
    {
        const auto k = criterion_kind(criterion, last_source);
        if (k == action_kind::unreadable) return k;
        if (k == action_kind::nonstandard) kind = k;
    }
    return kind;
}

// skipped_by is whether name skips source, one besides files and dns, as names other than those it answers do
// myhostname and mdns ones (as Go has it; every name meets the others).
bool skipped_by(std::string_view                      source,
                std::string_view                      name,
                const net::dns::config&               conf,
                const net::dns::detail::system_facts& facts)
{
    if (source == "myhostname")
    {
        // the machine's own name, and some others.
        if (is_localhost(name) || iequals(name, "_gateway") || iequals(name, "_outbound")) return false;

        const auto host = facts.host_name ? facts.host_name() : net::dns::detail::host_name();
        return !host.empty() && !iequals(name, host);
    }

    if (source.starts_with("mdns"))
    {
        // .local names (and those /etc/mdns.allow lists, which isn't read: any it allows).
        return !ends_with_ignore_case(name, ".local") && !conf.mdns_allow;
    }

    return false;
}

}

namespace net::dns::detail
{

std::optional<resolver::backend> fixed_choice(const system_facts& facts)
{
    if (const auto forced = env(facts, "NET_DNS"); forced.has_value())
    {
        if (*forced == "system") return backend::system;
        if (*forced == "native") return backend::native;
    }

    if (!facts.native_os.value_or(this_os_native)) return backend::system;

    // NOTE: LOCALDOMAIN changes the C library's search list even when it's empty (as Go has it).
    if (env(facts, "LOCALDOMAIN").has_value()) return backend::system;
    if (!env(facts, "RES_OPTIONS").value_or("").empty()) return backend::system;
    if (!env(facts, "HOSTALIASES").value_or("").empty()) return backend::system;

    return std::nullopt;
}

resolver::backend choose(const config& conf, std::string_view name, const system_facts& facts)
{
    if (conf.unknown_option) return backend::system;

    if (name.ends_with('.')) name.remove_suffix(1);

    // NOTE: no hosts sources (or no nsswitch.conf) is the hosts table, then DNS.
    const auto& sources = conf.nss_hosts;
    if (sources.empty()) return backend::native;

    bool files_or_dns = false;
    for (std::size_t i = 0; i < sources.size(); ++i)
    {
        const auto& source = sources[i];

        const auto kind = action_kind_of(source.action, i + 1 == sources.size());
        if (kind == action_kind::unreadable) return backend::system;

        if (source.name == "files" || source.name == "dns")
        {
            if (kind != action_kind::standard) return backend::system;
            files_or_dns = true;
        }
        else if (!skipped_by(source.name, name, conf, facts))
        {
            return backend::system;
        }
    }

    return files_or_dns ? backend::native : backend::system;
}

}
