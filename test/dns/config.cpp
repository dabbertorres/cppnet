#include "dns/config.hpp"

#include <chrono>
#include <expected>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "ip_addr.hpp"
#include "ipv4_addr.hpp"
#include "ipv6_addr.hpp"
#include "util/defer.hpp"

namespace dns = net::dns;

namespace
{

using namespace std::chrono_literals;

net::ip_addr addr(std::string_view text)
{
    auto parsed = net::ip_addr::parse(text);
    REQUIRE(parsed.has_value());
    return *parsed;
}

// default_servers are the nameservers a resolv.conf without any gives.
std::vector<dns::nameserver> default_servers()
{
    return {
        {.addr = net::ipv4_addr::loopback()},
        {.addr = net::ipv6_addr::loopback()},
    };
}

}

TEST_CASE("an empty resolv.conf gives the defaults", "[dns][config][resolv.conf]")
{
    const auto conf = dns::parse_resolv_conf("");

    CHECK(conf.nameservers == default_servers());
    CHECK(conf.search.empty());
    CHECK(conf.ndots == 1);
    CHECK(conf.timeout == 5s);
    CHECK(conf.attempts == 2);
    CHECK_FALSE(conf.rotate);
    CHECK_FALSE(conf.single_request);
    CHECK_FALSE(conf.single_request_reopen);
    CHECK_FALSE(conf.use_tcp);
    CHECK_FALSE(conf.trust_ad);
    CHECK_FALSE(conf.unknown_option);
    CHECK(conf.hosts.empty());
    CHECK(conf.order == dns::lookup_order::files_dns);
}

TEST_CASE("resolv.conf's nameservers are read, up to three", "[dns][config][resolv.conf]")
{
    const auto conf = dns::parse_resolv_conf(R"(# a comment
; another
nameserver 192.0.2.1
nameserver   2001:db8::1   # trailing words are ignored
nameserver
nameserver not-an-address
nameserver fe80::1%eth0
nameserver 192.0.2.2
nameserver 192.0.2.3
)");

    // the line without an address, the name, and the zone (which ip_addr can't hold) are skipped, and the fourth
    // address is one too many.
    const std::vector<dns::nameserver> expected{
        {.addr = addr("192.0.2.1")},
        {.addr = addr("2001:db8::1")},
        {.addr = addr("192.0.2.2")},
    };
    CHECK(conf.nameservers == expected);
    CHECK_FALSE(conf.unknown_option);
}

TEST_CASE("resolv.conf's search list is its last search or domain line", "[dns][config][resolv.conf]")
{
    struct test_case
    {
        std::string              text{};
        std::string              hostname{};
        std::vector<std::string> search{};
    };

    auto tc = GENERATE(
        test_case{
            .text   = "search a.example B.Example.\n",
            .search = {"a.example", "b.example"}
    },
        test_case{.text = "domain a.example\n", .search = {"a.example"}},
        test_case{.text = "search a.example b.example\ndomain c.example\n", .search = {"c.example"}},
        test_case{.text = "domain c.example\nsearch a.example b.example\n", .search = {"a.example", "b.example"}},
        // the root is dropped
        test_case{.text = "search . a.example\n", .search = {"a.example"}},
        // a search line of nothing (or the root) clears it, and the host name's domain doesn't apply
        test_case{.text = "search .\n", .hostname = "host.example", .search = {}},
        test_case{.text = "domain\n", .hostname = "host.example", .search = {"example"}},
        // without either, the host name's domain
        test_case{.text = "", .hostname = "host.Sub.Example", .search = {"sub.example"}},
        test_case{.text = "", .hostname = "host", .search = {}},
        test_case{.text = "", .hostname = ".example", .search = {}},
        test_case{.text = "", .hostname = "host.", .search = {}},
        test_case{.text = "search a.example\n", .hostname = "host.example", .search = {"a.example"}});
    CAPTURE(tc.text, tc.hostname);

    CHECK(dns::parse_resolv_conf(tc.text, tc.hostname).search == tc.search);
}

TEST_CASE("resolv.conf's options are read as Go reads them", "[dns][config][resolv.conf]")
{
    SECTION("all of them")
    {
        const auto conf = dns::parse_resolv_conf(
            "options ndots:3 timeout:2 attempts:4 rotate single-request single-request-reopen use-vc trust-ad edns0\n");

        CHECK(conf.ndots == 3);
        CHECK(conf.timeout == 2s);
        CHECK(conf.attempts == 4);
        CHECK(conf.rotate);
        CHECK(conf.single_request);
        CHECK(conf.single_request_reopen);
        CHECK(conf.use_tcp);
        CHECK(conf.trust_ad);
        CHECK_FALSE(conf.unknown_option);
    }

    SECTION("numbers")
    {
        struct test_case
        {
            std::string               text{};
            int                       ndots    = 1;
            std::chrono::milliseconds timeout  = 5s;
            int                       attempts = 2;
        };

        auto tc = GENERATE(
            // the limits
            test_case{.text = "options ndots:16 timeout:0 attempts:0", .ndots = 15, .timeout = 1s, .attempts = 1},
            test_case{.text = "options ndots:0", .ndots = 0},
            // leading digits, or none (0, then the minimum)
            test_case{.text = "options ndots:2x timeout:3s attempts:x", .ndots = 2, .timeout = 3s, .attempts = 1},
            test_case{.text = "options ndots: timeout:", .ndots = 0, .timeout = 1s},
            test_case{.text = "options ndots:99999999999999999999", .ndots = 15},
            // the last one wins, over lines
            test_case{.text = "options ndots:2\noptions ndots:4", .ndots = 4});
        CAPTURE(tc.text);

        const auto conf = dns::parse_resolv_conf(tc.text);
        CHECK(conf.ndots == tc.ndots);
        CHECK(conf.timeout == tc.timeout);
        CHECK(conf.attempts == tc.attempts);
        CHECK_FALSE(conf.unknown_option);
    }

    SECTION("use-vc's other names")
    {
        const auto* const name = GENERATE("use-vc", "usevc", "tcp");
        CAPTURE(name);

        CHECK(dns::parse_resolv_conf(std::string{"options "} + name).use_tcp);
    }

    SECTION("what it doesn't know")
    {
        const auto* const text = GENERATE("options no-reload",
                                          "options inet6",
                                          "options ndots:2 debug",
                                          "sortlist 130.155.160.0/255.255.240.0",
                                          "lookup file bind",
                                          "nameserver_typo 192.0.2.1");
        CAPTURE(text);

        const auto conf = dns::parse_resolv_conf(text);
        CHECK(conf.unknown_option);
    }

    SECTION("CRLF line ends and tabs")
    {
        const auto conf = dns::parse_resolv_conf("nameserver\t192.0.2.1\r\noptions\tndots:2\trotate\r\n");
        CHECK(conf.nameservers == std::vector<dns::nameserver>{{.addr = addr("192.0.2.1")}});
        CHECK(conf.ndots == 2);
        CHECK(conf.rotate);
        CHECK_FALSE(conf.unknown_option);
    }
}

TEST_CASE("a hosts file is read into names' addresses", "[dns][config][hosts]")
{
    const auto table = dns::parse_hosts(R"(# a comment
127.0.0.1	localhost Localhost.Localdomain.
::1		localhost ip6-localhost   # IPv6
192.0.2.1 www.example www.example # twice on a line
192.0.2.2 www.example
not-an-address bad.example
fe80::1%eth0 zoned.example
192.0.2.3
   # an indented comment
192.0.2.4 # names after a comment don't count
192.0.2.5 trailing.example.)");

    const dns::hosts_table expected{
        {            "localhost",                          {addr("127.0.0.1"), addr("::1")}},
        {"localhost.localdomain",                                       {addr("127.0.0.1")}},
        {        "ip6-localhost",                                             {addr("::1")}},
        {          "www.example", {addr("192.0.2.1"), addr("192.0.2.1"), addr("192.0.2.2")}},
        {     "trailing.example",                                       {addr("192.0.2.5")}},
    };
    CHECK(table == expected);
}

TEST_CASE("nsswitch.conf's hosts line orders files and DNS", "[dns][config][nsswitch]")
{
    struct test_case
    {
        std::string                  text{};
        std::vector<dns::nss_source> sources{};
        dns::lookup_order            order = dns::lookup_order::files_dns;
    };

    auto tc = GENERATE(
        test_case{
            .text    = "",
            .sources = {},
            .order   = dns::lookup_order::files_dns
    },
        test_case{.text = "passwd: files\n", .sources = {}, .order = dns::lookup_order::files_dns},
        test_case{.text    = "hosts: files dns\n",
                  .sources = {{.name = "files"}, {.name = "dns"}},
                  .order   = dns::lookup_order::files_dns},
        test_case{.text    = "hosts:dns\tfiles # a comment\n",
                  .sources = {{.name = "dns"}, {.name = "files"}},
                  .order   = dns::lookup_order::dns_files},
        test_case{.text = "hosts: files\n", .sources = {{.name = "files"}}, .order = dns::lookup_order::files},
        test_case{.text = "hosts: dns\n", .sources = {{.name = "dns"}}, .order = dns::lookup_order::dns},
        // Arch's default: other sources, with actions
        test_case{.text    = "hosts: mymachines resolve [!UNAVAIL=return] files myhostname dns\n",
                  .sources = {{.name = "mymachines"},
                              {.name = "resolve", .action = "[!UNAVAIL=return]"},
                              {.name = "files"},
                              {.name = "myhostname"},
                              {.name = "dns"}},
                  .order   = dns::lookup_order::files_dns},
        test_case{.text    = "hosts: dns[NOTFOUND=return] files\n",
                  .sources = {{.name = "dns", .action = "[NOTFOUND=return]"}, {.name = "files"}},
                  .order   = dns::lookup_order::dns_files},
        // the last hosts line wins
        test_case{.text    = "hosts: files dns\nhosts: dns\n",
                  .sources = {{.name = "dns"}},
                  .order   = dns::lookup_order::dns},
        test_case{.text = "hosts: mdns4_minimal\n", .sources = {{.name = "mdns4_minimal"}}});
    CAPTURE(tc.text);

    const auto sources = dns::parse_nsswitch_hosts(tc.text);
    CHECK(sources == tc.sources);
    CHECK(dns::order_of(sources) == tc.order);
}

TEST_CASE("the configuration is read from its files, and again when they change", "[dns][config][files]")
{
    const auto dir =
        std::filesystem::temp_directory_path()
        / ("cppnet-dns-config-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(dir);

    const net::util::defer cleanup{[&dir] noexcept
                                   {
                                       std::error_code ignored;
                                       std::filesystem::remove_all(dir, ignored);
                                   }};

    const auto write = [](const std::filesystem::path& path, std::string_view text)
    {
        std::ofstream out{path, std::ios::binary | std::ios::trunc};
        out << text;
    };

    const dns::detail::config_files files{
        .resolv_conf    = (dir / "resolv.conf").string(),
        .hosts          = (dir / "hosts").string(),
        .nsswitch_conf  = (dir / "nsswitch.conf").string(),
        .check_interval = 0s,
    };

    SECTION("missing files give the defaults")
    {
        dns::detail::config_cache cache{std::nullopt, files};

        const auto conf = cache.get();
        REQUIRE(conf != nullptr);
        CHECK(conf->nameservers == default_servers());
        CHECK(conf->hosts.empty());
        CHECK(conf->order == dns::lookup_order::files_dns);
    }

    SECTION("read, and read again")
    {
        write(files.resolv_conf, "nameserver 192.0.2.1\noptions ndots:2\n");
        write(files.hosts, "192.0.2.10 one.example\n");
        write(files.nsswitch_conf, "hosts: dns files\n");

        dns::detail::config_cache cache{std::nullopt, files};

        const auto first = cache.get();
        REQUIRE(first != nullptr);
        CHECK(first->nameservers == std::vector<dns::nameserver>{{.addr = addr("192.0.2.1")}});
        CHECK(first->ndots == 2);
        CHECK(first->hosts
              == dns::hosts_table{
                  {"one.example", {addr("192.0.2.10")}}
        });
        CHECK(first->order == dns::lookup_order::dns_files);

        // unchanged: the same one.
        CHECK(cache.get() == first);

        // a different size, so the change is seen whatever the file system's timestamps' resolution.
        write(files.hosts, "192.0.2.20 two.example\n192.0.2.21 two.example\n");

        const auto second = cache.get();
        REQUIRE(second != nullptr);
        CHECK(second != first);
        CHECK(second->hosts
              == dns::hosts_table{
                  {"two.example", {addr("192.0.2.20"), addr("192.0.2.21")}}
        });
        CHECK(second->ndots == 2);

        // the first is still whole, for whoever holds it.
        CHECK(first->hosts
              == dns::hosts_table{
                  {"one.example", {addr("192.0.2.10")}}
        });
    }

    SECTION("not checked again until the interval has passed")
    {
        write(files.hosts, "192.0.2.10 one.example\n");

        auto slow           = files;
        slow.check_interval = 1h;
        dns::detail::config_cache cache{std::nullopt, slow};

        const auto first = cache.get();
        write(files.hosts, "192.0.2.20 two.example\n192.0.2.21 two.example\n");
        CHECK(cache.get() == first);
    }

    SECTION("a given configuration is never read")
    {
        write(files.hosts, "192.0.2.10 one.example\n");

        dns::config given;
        given.ndots = 4;
        dns::detail::config_cache cache{given, files};

        const auto conf = cache.get();
        REQUIRE(conf != nullptr);
        CHECK(*conf == given);
    }
}
