#include "dns/automatic.hpp"

#include <atomic>
#include <chrono>
#include <compare>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "coro/task.hpp"
#include "coro/thread_pool.hpp"
#include "dns/resolver.hpp"
#include "io/scheduler.hpp"
#include "ip_addr.hpp"
#include "ipv4_addr.hpp"
#include "util/defer.hpp"

namespace dns = net::dns;

namespace
{

using namespace std::chrono_literals;

using net::coro::task;
using backend        = dns::resolver::backend;
using resolve_result = std::expected<std::vector<net::ip_addr>, std::error_code>;
using environment    = std::map<std::string, std::string, std::less<>>;

// facts_of are a machine's facts: its OS (whether the native back end reads its configuration), its environment (only
// what's given), and its name.
dns::detail::system_facts facts_of(bool native_os, environment env = {}, std::string host_name = "box.example")
{
    return {
        .native_os = native_os,
        .getenv    = [env = std::move(env)](const char* name) -> std::optional<std::string>
        {
            const auto it = env.find(std::string_view{name});
            if (it == env.end()) return std::nullopt;
            return it->second;
        },
        .host_name = [host_name = std::move(host_name)] { return host_name; },
    };
}

// config_of is the configuration resolv.conf's and nsswitch.conf's text give, with or without /etc/mdns.allow.
dns::config config_of(std::string_view nsswitch_conf, std::string_view resolv_conf = "", bool mdns_allow = false)
{
    auto conf       = dns::parse_resolv_conf(resolv_conf);
    conf.nss_hosts  = dns::parse_nsswitch_hosts(nsswitch_conf);
    conf.order      = dns::order_of(conf.nss_hosts);
    conf.mdns_allow = mdns_allow;
    return conf;
}

const net::ipv4_addr hosts_addr{192, 0, 2, 1};  // the hosts table's (the native back end's) answer
const net::ipv4_addr system_addr{192, 0, 2, 2}; // the system back end's

void write_file(const std::filesystem::path& path, std::string_view text)
{
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    out << text;
}

}

TEST_CASE("NET_DNS, the OS and the environment fix automatic's choice", "[dns][automatic]")
{
    struct test_case
    {
        bool                   native_os = true;
        environment            env{};
        std::optional<backend> expected{};
    };

    auto tc = GENERATE(
        // left to the configuration and the name
        test_case{
            .native_os = true,
            .env       = {},
            .expected  = std::nullopt
    },
        // an OS whose configuration the native back end doesn't read (e.g. macOS)
        test_case{.native_os = false, .env = {}, .expected = backend::system},
        // NET_DNS forces one, wherever
        test_case{.native_os = true, .env = {{"NET_DNS", "system"}}, .expected = backend::system},
        test_case{.native_os = false, .env = {{"NET_DNS", "native"}}, .expected = backend::native},
        test_case{.native_os = true,
                  .env       = {{"NET_DNS", "native"}, {"LOCALDOMAIN", "example"}},
                  .expected  = backend::native},
        // other values of it are ignored
        test_case{.native_os = true, .env = {{"NET_DNS", "go"}}, .expected = std::nullopt},
        test_case{.native_os = true, .env = {{"NET_DNS", ""}}, .expected = std::nullopt},
        test_case{.native_os = true, .env = {{"NET_DNS", "Native"}}, .expected = std::nullopt},
        test_case{.native_os = false, .env = {{"NET_DNS", "cgo"}}, .expected = backend::system},
        // what changes the C library's behavior: LOCALDOMAIN even empty, the others only if not
        test_case{.native_os = true, .env = {{"LOCALDOMAIN", ""}}, .expected = backend::system},
        test_case{.native_os = true, .env = {{"LOCALDOMAIN", "example"}}, .expected = backend::system},
        test_case{.native_os = true, .env = {{"RES_OPTIONS", "ndots:2"}}, .expected = backend::system},
        test_case{.native_os = true, .env = {{"RES_OPTIONS", ""}}, .expected = std::nullopt},
        test_case{.native_os = true, .env = {{"HOSTALIASES", "/etc/host.aliases"}}, .expected = backend::system},
        test_case{.native_os = true, .env = {{"HOSTALIASES", ""}}, .expected = std::nullopt});
    CAPTURE(tc.native_os, tc.env);

    CHECK(dns::detail::fixed_choice(facts_of(tc.native_os, tc.env)) == tc.expected);
}

TEST_CASE("automatic chooses from nsswitch.conf's hosts line, and the name", "[dns][automatic][nsswitch]")
{
    struct test_case
    {
        std::string nsswitch_conf{};
        std::string name{};
        backend     expected   = backend::native;
        bool        mdns_allow = false;
        std::string host_name  = "box.example";
    };

    // a desktop's (Debian's and Ubuntu's): mDNS for .local names, myhostname for the machine's own.
    const std::string desktop = "hosts: files mdns4_minimal [NOTFOUND=return] dns myhostname\n";

    auto tc = GENERATE_COPY(
        // none (no hosts line, or no nsswitch.conf): the hosts table, then DNS
        test_case{.nsswitch_conf = "", .name = "example.com"},
        test_case{.nsswitch_conf = "passwd: files\n", .name = "example.com"},
        test_case{.nsswitch_conf = "hosts:\n", .name = "example.com"},
        // a server's (Ubuntu's, FreeBSD's)
        test_case{.nsswitch_conf = "hosts: files dns\n", .name = "example.com"},
        test_case{.nsswitch_conf = "hosts: files dns\n", .name = "printer.local"},
        test_case{.nsswitch_conf = "hosts: files dns\n", .name = "box.example"},
        test_case{.nsswitch_conf = "hosts: dns files\n", .name = "example.com"},
        test_case{.nsswitch_conf = "hosts: files\n", .name = "example.com"},
        test_case{.nsswitch_conf = "hosts: dns\n", .name = "example.com"},
        // Arch's: systemd's sources, which the native back end doesn't have
        test_case{.nsswitch_conf = "hosts: mymachines resolve [!UNAVAIL=return] files myhostname dns\n",
                  .name          = "example.com",
                  .expected      = backend::system},
        // Fedora's
        test_case{.nsswitch_conf =
                      "hosts: files myhostname mdns4_minimal [NOTFOUND=return] resolve [!UNAVAIL=return] dns\n",
                  .name     = "example.com",
                  .expected = backend::system},
        test_case{.nsswitch_conf = "hosts: nis files dns\n", .name = "example.com", .expected = backend::system},
        // a desktop's: other names skip mDNS and myhostname
        test_case{.nsswitch_conf = desktop, .name = "example.com"},
        test_case{.nsswitch_conf = desktop, .name = "example.com.", .expected = backend::native},
        test_case{.nsswitch_conf = desktop, .name = "printer.local", .expected = backend::system},
        test_case{.nsswitch_conf = desktop, .name = "Printer.LOCAL.", .expected = backend::system},
        test_case{.nsswitch_conf = desktop, .name = "printer.locals"},
        test_case{.nsswitch_conf = desktop, .name = "box.example", .expected = backend::system},
        test_case{.nsswitch_conf = desktop, .name = "BOX.example.", .expected = backend::system},
        test_case{.nsswitch_conf = desktop, .name = "box"},
        test_case{.nsswitch_conf = desktop, .name = "localhost", .expected = backend::system},
        test_case{.nsswitch_conf = desktop, .name = "localhost.localdomain", .expected = backend::system},
        test_case{.nsswitch_conf = desktop, .name = "app.localhost", .expected = backend::system},
        test_case{.nsswitch_conf = desktop, .name = "_gateway", .expected = backend::system},
        test_case{.nsswitch_conf = desktop, .name = "_outbound", .expected = backend::system},
        // the machine's name unknown: any name may be it
        test_case{.nsswitch_conf = desktop, .name = "example.com", .expected = backend::system, .host_name = ""},
        // /etc/mdns.allow may allow other names than .local ones
        test_case{.nsswitch_conf = desktop, .name = "example.com", .expected = backend::system, .mdns_allow = true},
        test_case{.nsswitch_conf = "hosts: files dns\n", .name = "example.com", .mdns_allow = true},
        // neither files nor dns
        test_case{.nsswitch_conf = "hosts: mdns4_minimal\n", .name = "example.com", .expected = backend::system},
        test_case{.nsswitch_conf = "hosts: myhostname\n", .name = "example.com", .expected = backend::system},
        // actions: each criterion its status's default (any case), or return on the last source
        test_case{.nsswitch_conf = "hosts: files [SUCCESS=return] dns\n", .name = "example.com"},
        test_case{.nsswitch_conf = "hosts: files [ notfound=continue UNAVAIL=continue tryagain=continue ] dns\n",
                  .name          = "example.com"},
        test_case{.nsswitch_conf = "hosts: files dns [NOTFOUND=return]\n", .name = "example.com"},
        test_case{.nsswitch_conf = "hosts: files []dns\n", .name = "example.com"},
        // ... Go's "debian_squeeze" case
        test_case{.nsswitch_conf = "hosts: dns [success=return notfound=continue unavail=continue tryagain=continue] "
                                   "files [notfound=return]\n",
                  .name          = "example.com"},
        // ... and not
        test_case{.nsswitch_conf = "hosts: files [NOTFOUND=return] dns\n",
                  .name          = "example.com",
                  .expected      = backend::system},
        test_case{.nsswitch_conf = "hosts: dns [!UNAVAIL=return] files\n",
                  .name          = "example.com",
                  .expected      = backend::system},
        test_case{.nsswitch_conf = "hosts: files dns [!UNAVAIL=continue]\n",
                  .name          = "example.com",
                  .expected      = backend::system},
        test_case{.nsswitch_conf = "hosts: files [SUCCESS=continue] dns\n",
                  .name          = "example.com",
                  .expected      = backend::system},
        test_case{.nsswitch_conf = "hosts: files dns [FOUND=return]\n",
                  .name          = "example.com",
                  .expected      = backend::system},
        test_case{.nsswitch_conf = "hosts: files [UNAVAIL=continue][NOTFOUND=continue] dns\n",
                  .name          = "example.com",
                  .expected      = backend::system},
        // actions that can't be read, wherever they are
        test_case{.nsswitch_conf = "hosts: files [NOTFOUND=continue dns\n",
                  .name          = "example.com",
                  .expected      = backend::system},
        test_case{.nsswitch_conf = "hosts: files [NOTFOUND] dns\n", .name = "example.com", .expected = backend::system},
        test_case{.nsswitch_conf = "hosts: files mdns4_minimal [!=] dns\n",
                  .name          = "example.com",
                  .expected      = backend::system});
    CAPTURE(tc.nsswitch_conf, tc.name, tc.mdns_allow, tc.host_name);

    const auto conf = config_of(tc.nsswitch_conf, "", tc.mdns_allow);
    CHECK(dns::detail::choose(conf, tc.name, facts_of(true, {}, tc.host_name)) == tc.expected);
}

TEST_CASE("automatic chooses the system's for what resolv.conf has that the native doesn't know", "[dns][automatic]")
{
    struct test_case
    {
        std::string resolv_conf{};
        backend     expected = backend::native;
    };

    auto tc = GENERATE(test_case{.resolv_conf = "nameserver 192.0.2.53\noptions ndots:2 rotate edns0\n"},
                       test_case{.resolv_conf = "options no-reload\n", .expected = backend::system},
                       test_case{.resolv_conf = "sortlist 130.155.160.0/255.255.240.0\n", .expected = backend::system},
                       // OpenBSD's ordering of the hosts table and DNS
                       test_case{.resolv_conf = "lookup file bind\n", .expected = backend::system});
    CAPTURE(tc.resolv_conf);

    const auto conf = config_of("hosts: files dns\n", tc.resolv_conf);
    CHECK(dns::detail::choose(conf, "example.com", facts_of(true)) == tc.expected);
}

TEST_CASE("automatic looks names up with the back end it chooses", "[dns][automatic][resolver]")
{
    struct test_case
    {
        backend      use       = backend::automatic;
        bool         native_os = true;
        environment  env{};
        std::string  nsswitch_conf{};
        std::string  host{};
        net::ip_addr expected = hosts_addr;
    };

    const std::string desktop = "hosts: files mdns4_minimal [NOTFOUND=return] dns myhostname\n";
    const std::string arch    = "hosts: mymachines resolve [!UNAVAIL=return] files myhostname dns\n";

    auto tc = GENERATE_COPY(
        test_case{.nsswitch_conf = desktop, .host = "native.test", .expected = hosts_addr},
        test_case{.nsswitch_conf = desktop, .host = "printer.local", .expected = system_addr},
        test_case{.nsswitch_conf = arch, .host = "native.test", .expected = system_addr},
        test_case{.native_os = false, .nsswitch_conf = desktop, .host = "native.test", .expected = system_addr},
        test_case{.native_os     = false,
                  .env           = {{"NET_DNS", "native"}},
                  .nsswitch_conf = arch,
                  .host          = "native.test",
                  .expected      = hosts_addr},
        test_case{.env           = {{"RES_OPTIONS", "ndots:2"}},
                  .nsswitch_conf = desktop,
                  .host          = "native.test",
                  .expected      = system_addr},
        // a back end chosen in the options wins, over NET_DNS too
        test_case{.use           = backend::system,
                  .env           = {{"NET_DNS", "native"}},
                  .nsswitch_conf = desktop,
                  .host          = "native.test",
                  .expected      = system_addr},
        test_case{.use = backend::native, .nsswitch_conf = arch, .host = "printer.local", .expected = hosts_addr});
    CAPTURE(tc.use, tc.native_os, tc.env, tc.nsswitch_conf, tc.host);

    // the hosts table has both names (and there are no nameservers): the native back end answers from it.
    auto conf  = config_of(tc.nsswitch_conf);
    conf.hosts = {
        {  "native.test", {hosts_addr}},
        {"printer.local", {hosts_addr}},
    };
    conf.nameservers.clear();

    std::atomic<int>    system_lookups{0};
    const dns::resolver resolver{
        {
         .use           = tc.use,
         .conf          = std::move(conf),
         .system_lookup = [&system_lookups](const std::string& /*host*/, net::protocol /*proto*/) -> resolve_result
            {
                system_lookups.fetch_add(1);
                return std::vector<net::ip_addr>{system_addr};
            }, .facts = facts_of(tc.native_os, tc.env),
         }
    };

    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(2)};

    auto result = sched.run_to_completion(resolver.resolve(&sched, tc.host));
    REQUIRE(result.has_value());
    CHECK(*result == std::vector<net::ip_addr>{tc.expected});
    CHECK(system_lookups.load() == (tc.expected == net::ip_addr{system_addr} ? 1 : 0));
}

TEST_CASE("automatic's choice follows the configuration's changes, and lookups of one name with different back ends "
          "aren't shared",
          "[dns][automatic][resolver][merge]")
{
    const auto dir =
        std::filesystem::temp_directory_path()
        / ("cppnet-dns-automatic-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(dir);

    const net::util::defer cleanup{[&dir] noexcept
                                   {
                                       std::error_code ignored;
                                       std::filesystem::remove_all(dir, ignored);
                                   }};

    const dns::detail::config_files files{
        .resolv_conf    = (dir / "resolv.conf").string(),
        .hosts          = (dir / "hosts").string(),
        .nsswitch_conf  = (dir / "nsswitch.conf").string(),
        .mdns_allow     = (dir / "mdns.allow").string(),
        .check_interval = 0s,
    };

    // the system's first; the native's once nsswitch.conf changes (its size too, so the change is seen whatever the
    // file system's timestamps' resolution).
    write_file(files.hosts, "192.0.2.1 moving.test\n");
    write_file(files.nsswitch_conf, "hosts: resolve files dns\n");

    // the system's lookups block until released.
    std::atomic<int>  started{0};
    std::atomic<bool> released{false};

    const dns::resolver resolver{
        {
         .system_lookup = [&](const std::string& /*host*/, net::protocol /*proto*/) -> resolve_result
            {
                started.fetch_add(1);
                released.wait(false);
                return std::vector<net::ip_addr>{system_addr};
            }, .files = files,
         .facts = facts_of(true),
         }
    };

    // the first caller's result, and whether it has it.
    struct first_caller
    {
        resolve_result             result{std::unexpected(std::error_code{})};
        std::atomic<std::uint64_t> done{0};
    };
    first_caller first;

    net::io::scheduler     sched{std::make_shared<net::coro::thread_pool>(2)};
    const net::util::defer release{[&released] noexcept
                                   {
                                       released.store(true);
                                       released.notify_all();
                                   }};

    const auto test = [](net::io::scheduler*              sched,
                         const dns::resolver*             resolver,
                         const dns::detail::config_files* files,
                         std::atomic<int>*                started,
                         std::atomic<bool>*               released,
                         first_caller*                    first) -> task<resolve_result>
    {
        const auto call = [](net::io::scheduler* sched, const dns::resolver* resolver, first_caller* out) -> task<>
        {
            out->result = co_await resolver->resolve(sched, "moving.test");
            out->done.store(1);
            sched->notify_all(out->done);
        };
        REQUIRE(sched->spawn(call(sched, resolver, first)));

        // the system's lookup has started, and waits.
        const auto until = net::io::clock::now() + 10s;
        while (started->load() < 1)
        {
            REQUIRE(net::io::clock::now() < until);
            const auto err = co_await sched->sleep_until(net::io::clock::now() + 1ms);
            REQUIRE_FALSE(err);
        }

        // the native's, now: not the system's lookup in flight, which is still blocked (and would hold it until the
        // deadline).
        write_file(files->nsswitch_conf, "hosts: files dns\n");
        auto second = co_await resolver->resolve(sched, "MOVING.test", {.deadline = net::io::clock::now() + 5s});

        released->store(true);
        released->notify_all();
        while (first->done.load() == 0)
        {
            const auto err = co_await sched->wait_on(first->done, 0, until);
            REQUIRE_FALSE(err);
        }
        co_return second;
    };

    auto second = sched.run_to_completion(test(&sched, &resolver, &files, &started, &released, &first));
    CHECK(second == resolve_result{std::vector<net::ip_addr>{hosts_addr}});
    CHECK(first.result == resolve_result{std::vector<net::ip_addr>{system_addr}});
    CHECK(started.load() == 1);
}
