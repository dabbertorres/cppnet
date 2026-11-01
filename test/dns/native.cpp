#include "dns/native.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <ratio>
#include <set>
#include <stop_token>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "coro/task.hpp"
#include "coro/thread_pool.hpp"
#include "dns/config.hpp"
#include "dns/fake_server.hpp"
#include "dns/message.hpp"
#include "dns/resolver.hpp"
#include "io/scheduler.hpp"
#include "ip_addr.hpp"
#include "ipv4_addr.hpp"
#include "ipv6_addr.hpp"
#include "udp.hpp"

namespace dns = net::dns;

namespace
{

using namespace std::chrono_literals;

using net::coro::task;
using resolve_result = std::expected<std::vector<net::ip_addr>, std::error_code>;

std::shared_ptr<net::coro::thread_pool> make_pool() { return std::make_shared<net::coro::thread_pool>(2); }

// config_for is a configuration of servers, without a search list or hosts, with a timeout (long, unless a test waits
// for it to pass) and attempts.
dns::config config_for(std::vector<dns::nameserver> servers, std::chrono::milliseconds timeout = 10s, int attempts = 1)
{
    dns::config conf;
    conf.nameservers = std::move(servers);
    conf.timeout     = timeout;
    conf.attempts    = attempts;
    return conf;
}

dns::resolver native(dns::config conf)
{
    return dns::resolver{
        {.use = dns::resolver::backend::native, .conf = std::move(conf)}
    };
}

// timed is a resolution's result, and how long it took.
struct timed
{
    resolve_result           result{std::unexpected(std::error_code{})};
    net::io::clock::duration elapsed{};

    // what went wrong in the test's own steps, if anything (for the test thread to check: Catch2's assertions aren't
    // for the scheduler's threads).
    std::string failed{};
};

task<timed> timed_resolve(net::io::scheduler*  sched,
                          const dns::resolver* resolver,
                          std::string          host,
                          dns::resolve_options options = {})
{
    const auto start  = net::io::clock::now();
    auto       result = co_await resolver->resolve(sched, std::move(host), std::move(options));
    co_return timed{.result = std::move(result), .elapsed = net::io::clock::now() - start};
}

// set_of is addrs, in an order of their own, to compare what doesn't depend on RFC 6724's order (which depends on the
// machine's addresses).
std::set<std::string> set_of(const std::vector<net::ip_addr>& addrs)
{
    std::set<std::string> out;
    for (const auto& addr : addrs) out.insert(addr.to_string());
    return out;
}

// wait_received waits (in a task) until server has received n queries, returning whether it has: not if the
// scheduler's stopped, or it hasn't within 10 seconds.
task<bool> wait_received(net::io::scheduler* sched, const test::fake_server* server, std::uint64_t n)
{
    const auto until = net::io::clock::now() + 10s;
    for (auto seen = server->received().load(); seen < n; seen = server->received().load())
    {
        if (auto err = co_await sched->wait_on(server->received(), seen, until); err) co_return false;
    }
    co_return true;
}

// drop answers nothing.
std::vector<test::fake_reply> drop(const test::fake_query& /*query*/) { return {}; }

const net::ipv4_addr addr4{192, 0, 2, 1};
const net::ipv6_addr addr6{0x2001, 0xdb8, 0, 0, 0, 0, 0, 1};

// host_zone has host.test's addresses, one of each family.
test::zone host_zone()
{
    return {
        {"host.test", {test::a("host.test", addr4), test::aaaa("host.test", addr6)}},
    };
}

}

TEST_CASE("names are tried as the search list and ndots say", "[dns][native][names]")
{
    struct test_case
    {
        std::string              name{};
        int                      ndots = 1;
        std::vector<std::string> expected{};
    };

    auto tc = GENERATE(
        test_case{
            .name     = "host",
            .expected = {"host.a.test", "host.b.test", "host"}
    },
        test_case{.name = "x.y", .expected = {"x.y", "x.y.a.test", "x.y.b.test"}},
        test_case{.name = "x.y", .ndots = 2, .expected = {"x.y.a.test", "x.y.b.test", "x.y"}},
        test_case{.name = "host", .ndots = 0, .expected = {"host", "host.a.test", "host.b.test"}},
        // a final dot: only as it is
        test_case{.name = "host.", .expected = {"host"}},
        test_case{.name = "x.y.", .ndots = 5, .expected = {"x.y"}},
        // .onion isn't asked of DNS (RFC 7686), but under a search domain it's another name (as Go has it)
        test_case{.name = "secret.onion", .expected = {"secret.onion.a.test", "secret.onion.b.test"}},
        test_case{.name = "secret.ONION.", .expected = {}},
        test_case{.name = "onion", .expected = {"onion.a.test", "onion.b.test", "onion"}});
    CAPTURE(tc.name, tc.ndots);

    dns::config conf;
    conf.search = {"a.test", "b.test"};
    conf.ndots  = tc.ndots;

    CHECK(dns::detail::name_list(conf, tc.name) == tc.expected);
}

TEST_CASE("names too long under a search domain aren't tried under it", "[dns][native][names]")
{
    const std::string label(63, 'a');
    const auto        long_name = label + "." + label + "." + label + "." + std::string(55, 'a'); // 246 bytes

    dns::config conf;
    conf.search = {"short", "longer.domain", "bad..domain"};

    // 246 + 6 fits in 253, and 246 + 14 doesn't; the empty label isn't sent.
    CHECK(dns::detail::name_list(conf, long_name) == std::vector{long_name, long_name + ".short"});
}

TEST_CASE("query IDs are random", "[dns][native][id]")
{
    std::set<std::uint16_t> ids;
    for (int i = 0; i < 256; ++i) ids.insert(dns::detail::random_id());

    // 256 draws of 65536 values: a handful of repeats at most, by chance.
    CHECK(ids.size() > 240);
}

TEST_CASE("addresses are sorted by RFC 6724", "[dns][native][rfc6724]")
{
    const auto ip = [](std::string_view text)
    {
        auto addr = net::ip_addr::parse(text);
        REQUIRE(addr.has_value());
        return *addr;
    };

    struct test_case
    {
        std::vector<std::string>                in{};
        std::vector<std::optional<std::string>> sources{};
        std::vector<std::string>                expected{};
        bool                                    reverse = true; // whether the reversed input sorts the same
    };

    // Go's addrselect_test.go's cases, and one with an address that can't be reached.
    auto tc = GENERATE(
        // prefer a matching scope
        test_case{
            .in       = {"2001:db8:1::1", "198.51.100.121"},
            .sources  = {"2001:db8:1::2",  "169.254.13.78"},
            .expected = {"2001:db8:1::1", "198.51.100.121"}
    },
        test_case{.in       = {"2001:db8:1::1", "198.51.100.121"},
                  .sources  = {"fe80::1", "198.51.100.117"},
                  .expected = {"198.51.100.121", "2001:db8:1::1"}},
        // prefer higher precedence
        test_case{.in       = {"2001:db8:1::1", "10.1.2.3"},
                  .sources  = {"2001:db8:1::2", "10.1.2.4"},
                  .expected = {"2001:db8:1::1", "10.1.2.3"}},
        // prefer a smaller scope
        test_case{.in       = {"2001:db8:1::1", "fe80::1"},
                  .sources  = {"2001:db8:1::2", "fe80::2"},
                  .expected = {"fe80::1", "2001:db8:1::1"}},
        // Go's issue 13283: a 10/8 source doesn't prefer 23/8 destinations (no prefix matching for IPv4)
        test_case{
            .in = {"54.83.193.112", "184.72.238.214", "23.23.172.185", "75.101.148.21", "23.23.134.56", "23.21.50.150"},
            .sources = {"10.2.3.4", "10.2.3.4", "10.2.3.4", "10.2.3.4", "10.2.3.4", "10.2.3.4"},
            .expected =
                {"54.83.193.112", "184.72.238.214", "23.23.172.185", "75.101.148.21", "23.23.134.56", "23.21.50.150"},
            .reverse = false},
        // the longest matching prefix, for IPv6
        test_case{.in       = {"2001:db8:2::1", "2001:db8:1::1"},
                  .sources  = {"2001:db8:1::2", "2001:db8:1::2"},
                  .expected = {"2001:db8:1::1", "2001:db8:2::1"}},
        // avoid unusable destinations
        test_case{.in       = {"2001:db8:1::1", "192.0.2.1"},
                  .sources  = {std::nullopt, "192.0.2.2"},
                  .expected = {"192.0.2.1", "2001:db8:1::1"}},
        // loopback's precedence over IPv4's
        test_case{.in = {"127.0.0.1", "::1"}, .sources = {"127.0.0.1", "::1"}, .expected = {"::1", "127.0.0.1"}});
    CAPTURE(tc.in);

    std::vector<net::ip_addr>                addrs;
    std::vector<std::optional<net::ip_addr>> sources;
    std::vector<net::ip_addr>                expected;
    addrs.reserve(tc.in.size());
    sources.reserve(tc.sources.size());
    expected.reserve(tc.expected.size());
    for (const auto& a : tc.in) addrs.push_back(ip(a));
    for (const auto& s : tc.sources) sources.push_back(s.has_value() ? std::optional{ip(*s)} : std::nullopt);
    for (const auto& e : tc.expected) expected.push_back(ip(e));

    auto sorted = addrs;
    dns::detail::sort_by_rfc6724(sorted, sources);
    CHECK(sorted == expected);

    if (tc.reverse)
    {
        std::ranges::reverse(addrs);
        std::ranges::reverse(sources);
        dns::detail::sort_by_rfc6724(addrs, sources);
        CHECK(addrs == expected);
    }
}

TEST_CASE("addresses are sorted with the OS's source addresses", "[dns][native][rfc6724]")
{
    net::io::scheduler sched{make_pool()};

    // ::1 goes first (a higher precedence than IPv4's), if the machine has it: otherwise it can't be reached.
    const bool has_ipv6 = net::udp_socket::bind(&sched, net::ipv6_addr::loopback(), 0).has_value();

    std::vector<net::ip_addr> addrs{net::ipv4_addr::loopback(), net::ipv6_addr::loopback()};
    dns::detail::sort_by_rfc6724(addrs);

    const std::vector<net::ip_addr> expected =
        has_ipv6 ? std::vector<net::ip_addr>{net::ipv6_addr::loopback(), net::ipv4_addr::loopback()}
                 : std::vector<net::ip_addr>{net::ipv4_addr::loopback(), net::ipv6_addr::loopback()};
    CHECK(addrs == expected);
}

TEST_CASE("A and AAAA are asked at once, on one socket", "[dns][native][udp]")
{
    net::io::scheduler sched{make_pool()};
    test::fake_server  server{&sched, test::answer_from(host_zone())};

    const auto resolver = native(config_for({server.nameserver()}));

    auto out = sched.run_to_completion(timed_resolve(&sched, &resolver, "Host.Test"));

    REQUIRE(out.result.has_value());
    CHECK(set_of(*out.result) == set_of({addr4, addr6}));

    const auto queries = server.queries();
    REQUIRE(queries.size() == 2);

    const auto& first  = queries[0];
    const auto& second = queries[1];
    CHECK(first.msg.questions.front().type == dns::type::a);
    CHECK(second.msg.questions.front().type == dns::type::aaaa);
    CHECK(first.port == second.port);
    CHECK(first.msg.header.id != second.msg.header.id);

    for (const auto& q : queries)
    {
        CHECK_FALSE(q.tcp);
        CHECK(q.msg.header.recursion_desired);
        CHECK_FALSE(q.msg.header.authentic_data);
        REQUIRE(q.msg.questions.size() == 1);
        CHECK(q.msg.questions.front().name == "host.test");
        REQUIRE(q.msg.edns.has_value());
        CHECK(q.msg.edns->udp_size == 1232);
        CHECK_FALSE(q.msg.edns->dnssec_ok);
    }
}

TEST_CASE("only the families asked for are queried", "[dns][native][udp]")
{
    struct test_case
    {
        net::protocol             proto = net::protocol::not_care;
        dns::type                 qtype = dns::type::a;
        std::vector<net::ip_addr> expected{};
    };

    auto tc = GENERATE(test_case{.proto = net::protocol::ipv4, .qtype = dns::type::a, .expected = {addr4}},
                       test_case{.proto = net::protocol::ipv6, .qtype = dns::type::aaaa, .expected = {addr6}});
    CAPTURE(tc.proto);

    net::io::scheduler sched{make_pool()};
    test::fake_server  server{&sched, test::answer_from(host_zone())};

    const auto resolver = native(config_for({server.nameserver()}));

    auto out = sched.run_to_completion(timed_resolve(&sched, &resolver, "host.test", {.proto = tc.proto}));
    CHECK(out.result == tc.expected);

    const auto queries = server.queries();
    REQUIRE(queries.size() == 1);
    CHECK(queries[0].msg.questions.front().type == tc.qtype);
}

TEST_CASE("trust-ad sets the AD bit", "[dns][native][udp]")
{
    net::io::scheduler sched{make_pool()};
    test::fake_server  server{&sched, test::answer_from(host_zone())};

    auto conf     = config_for({server.nameserver()});
    conf.trust_ad = true;

    const auto resolver = native(conf);
    auto out = sched.run_to_completion(timed_resolve(&sched, &resolver, "host.test", {.proto = net::protocol::ipv4}));
    CHECK(out.result == std::vector<net::ip_addr>{addr4});

    const auto queries = server.queries();
    REQUIRE(queries.size() == 1);
    CHECK(queries[0].msg.header.authentic_data);
}

TEST_CASE("single-request asks A and AAAA in turn", "[dns][native][udp]")
{
    // A is answered only once AAAA has been asked (the answer going to the socket AAAA's came from): asked at once, A
    // is answered; asked in turn, AAAA is asked only once A's timeout has passed, and A is never answered.
    constexpr auto timeout = 300ms;

    std::optional<dns::message> asked_a; // the handler's calls are one at a time
    const auto answer_a_after_aaaa = [&asked_a](const test::fake_query& q) -> std::vector<test::fake_reply>
    {
        if (q.msg.questions.front().type == dns::type::a)
        {
            asked_a = q.msg;
            return {};
        }

        std::vector<test::fake_reply> replies{{.msg = test::reply_to(q.msg, {test::aaaa("host.test", addr6)})}};
        if (asked_a.has_value()) replies.push_back({.msg = test::reply_to(*asked_a, {test::a("host.test", addr4)})});
        return replies;
    };

    struct test_case
    {
        bool single_request        = false;
        bool single_request_reopen = false;
    };

    auto tc = GENERATE(test_case{.single_request = true}, test_case{.single_request_reopen = true}, test_case{});
    CAPTURE(tc.single_request, tc.single_request_reopen);

    net::io::scheduler sched{make_pool()};
    test::fake_server  server{&sched, answer_a_after_aaaa};

    auto conf                  = config_for({server.nameserver()}, timeout);
    conf.single_request        = tc.single_request;
    conf.single_request_reopen = tc.single_request_reopen;

    const auto resolver = native(conf);
    auto       out      = sched.run_to_completion(timed_resolve(&sched, &resolver, "host.test"));

    const auto queries = server.queries();
    REQUIRE(queries.size() == 2);
    CHECK(queries[0].msg.questions.front().type == dns::type::a);
    CHECK(queries[1].msg.questions.front().type == dns::type::aaaa);

    if (tc.single_request || tc.single_request_reopen)
    {
        // A failed, but AAAA was answered: asked once A's timeout had passed (as the server saw them: the sends' times
        // are a little apart from the timeout's start, so half of it tells).
        CHECK(out.result == std::vector<net::ip_addr>{addr6});
        CHECK(queries[1].when - queries[0].when >= timeout / 2);
    }
    else
    {
        // A was answered too, as AAAA was asked while A's question was open.
        REQUIRE(out.result.has_value());
        CHECK(set_of(*out.result) == set_of({addr4, addr6}));
    }

    // a socket of its own, only with single-request-reopen.
    if (tc.single_request_reopen) CHECK(queries[0].port != queries[1].port);
    else CHECK(queries[0].port == queries[1].port);
}

TEST_CASE("a truncated answer is asked again over TCP", "[dns][native][tcp]")
{
    const std::vector<dns::record> many{
        test::a("host.test", net::ipv4_addr{192, 0, 2, 1}),
        test::a("host.test", net::ipv4_addr{192, 0, 2, 2}),
        test::a("host.test", net::ipv4_addr{192, 0, 2, 3}),
    };

    const auto truncate_udp = [&many](const test::fake_query& q) -> std::vector<test::fake_reply>
    {
        if (q.tcp) return {{.msg = test::reply_to(q.msg, many)}};

        // the first, and TC.
        auto reply             = test::reply_to(q.msg, {many.front()});
        reply.header.truncated = true;
        return {{.msg = std::move(reply)}};
    };

    net::io::scheduler sched{make_pool()};
    test::fake_server  server{&sched, truncate_udp};

    const auto resolver = native(config_for({server.nameserver()}));
    auto out = sched.run_to_completion(timed_resolve(&sched, &resolver, "host.test", {.proto = net::protocol::ipv4}));

    REQUIRE(out.result.has_value());
    CHECK(set_of(*out.result)
          == set_of({
              net::ipv4_addr{192, 0, 2, 1},
              net::ipv4_addr{192, 0, 2, 2},
              net::ipv4_addr{192, 0, 2, 3}
    }));

    const auto queries = server.queries();
    REQUIRE(queries.size() == 2);
    CHECK_FALSE(queries[0].tcp);
    CHECK(queries[1].tcp);
    CHECK(queries[1].msg.questions == queries[0].msg.questions);
}

TEST_CASE("use-vc asks over TCP only", "[dns][native][tcp]")
{
    net::io::scheduler sched{make_pool()};
    test::fake_server  server{&sched, test::answer_from(host_zone())};

    auto conf    = config_for({server.nameserver()});
    conf.use_tcp = true;

    const auto resolver = native(conf);
    auto       out      = sched.run_to_completion(timed_resolve(&sched, &resolver, "host.test"));

    REQUIRE(out.result.has_value());
    CHECK(set_of(*out.result) == set_of({addr4, addr6}));

    const auto queries = server.queries();
    REQUIRE(queries.size() == 2);
    for (const auto& q : queries) CHECK(q.tcp);
}

TEST_CASE("a FORMERR to EDNS(0) is asked again without it", "[dns][native][edns]")
{
    struct test_case
    {
        bool tcp = false;
    };

    auto tc = GENERATE(test_case{.tcp = false}, test_case{.tcp = true});
    CAPTURE(tc.tcp);

    // a server from before EDNS(0).
    const auto old_server = [](const test::fake_query& q) -> std::vector<test::fake_reply>
    {
        if (q.msg.edns.has_value())
        {
            auto reply = test::reply_to(q.msg, {}, dns::rcode::format_error);
            reply.edns.reset();
            return {{.msg = std::move(reply)}};
        }
        return {{.msg = test::reply_to(q.msg, {test::a("host.test", addr4)})}};
    };

    net::io::scheduler sched{make_pool()};
    test::fake_server  server{&sched, old_server};

    auto conf    = config_for({server.nameserver()});
    conf.use_tcp = tc.tcp;

    const auto resolver = native(conf);
    auto out = sched.run_to_completion(timed_resolve(&sched, &resolver, "host.test", {.proto = net::protocol::ipv4}));
    CHECK(out.result == std::vector<net::ip_addr>{addr4});

    const auto queries = server.queries();
    REQUIRE(queries.size() == 2);
    CHECK(queries[0].msg.edns.has_value());
    CHECK_FALSE(queries[1].msg.edns.has_value());
    CHECK(queries[0].port == queries[1].port); // the same socket (or connection)
}

TEST_CASE("a FORMERR without EDNS(0) is a server failure", "[dns][native][edns]")
{
    const auto formerr = [](const test::fake_query& q) -> std::vector<test::fake_reply>
    { return {{.msg = test::reply_to(q.msg, {}, dns::rcode::format_error)}}; };

    net::io::scheduler sched{make_pool()};
    test::fake_server  server{&sched, formerr};

    const auto resolver = native(config_for({server.nameserver()}));
    auto out = sched.run_to_completion(timed_resolve(&sched, &resolver, "host.test", {.proto = net::protocol::ipv4}));

    REQUIRE_FALSE(out.result.has_value());
    CHECK(out.result.error() == dns::errc::server_failure);
    CHECK(server.queries().size() == 2);
}

TEST_CASE("replies that don't answer the query are read past", "[dns][native][udp]")
{
    const auto forger = [](const test::fake_query& q) -> std::vector<test::fake_reply>
    {
        const auto forged = test::a("host.test", net::ipv4_addr{203, 0, 113, 66});

        auto wrong_id = test::reply_to(q.msg, {forged});
        wrong_id.header.id ^= 1U;

        auto wrong_name = test::reply_to(q.msg, {test::a("other.test", net::ipv4_addr{203, 0, 113, 67})});
        wrong_name.questions.front().name = "other.test";

        auto wrong_type                   = test::reply_to(q.msg, {forged});
        wrong_type.questions.front().type = dns::type::aaaa;

        auto not_response            = test::reply_to(q.msg, {forged});
        not_response.header.response = false;

        auto two_questions = test::reply_to(q.msg, {forged});
        two_questions.questions.push_back(two_questions.questions.front());

        return {
            {.msg = std::move(wrong_id)},
            {.msg = std::move(wrong_name)},
            {.msg = std::move(wrong_type)},
            {.msg = std::move(not_response)},
            {.msg = std::move(two_questions)},
            {.msg = test::reply_to(q.msg, {test::a("HOST.test", addr4)})}, // the case of the name doesn't matter
        };
    };

    net::io::scheduler sched{make_pool()};
    test::fake_server  server{&sched, forger};

    const auto resolver = native(config_for({server.nameserver()}));
    auto out = sched.run_to_completion(timed_resolve(&sched, &resolver, "host.test", {.proto = net::protocol::ipv4}));

    CHECK(out.result == std::vector<net::ip_addr>{addr4});
    CHECK(server.queries().size() == 1);
}

TEST_CASE("an attempt that isn't answered in time is made again", "[dns][native][retry]")
{
    constexpr auto timeout = 300ms;

    // the first query of each type is dropped.
    std::set<dns::type> seen;
    const auto          drop_first = [&seen](const test::fake_query& q) -> std::vector<test::fake_reply>
    {
        const auto type = q.msg.questions.front().type;
        if (seen.insert(type).second) return {};
        return test::answer_from(host_zone())(q);
    };

    net::io::scheduler sched{make_pool()};
    test::fake_server  server{&sched, drop_first};

    const auto resolver = native(config_for({server.nameserver()}, timeout, 2));
    auto       out      = sched.run_to_completion(timed_resolve(&sched, &resolver, "host.test"));

    REQUIRE(out.result.has_value());
    CHECK(set_of(*out.result) == set_of({addr4, addr6}));
    CHECK(out.elapsed >= timeout);

    // the second attempt on a socket of its own.
    const auto queries = server.queries();
    REQUIRE(queries.size() == 4);
    CHECK(queries[0].port == queries[1].port);
    CHECK(queries[2].port == queries[3].port);
    CHECK(queries[0].port != queries[2].port);
}

TEST_CASE("a server that doesn't answer is followed by the next", "[dns][native][retry]")
{
    constexpr auto timeout = 300ms;

    net::io::scheduler sched{make_pool()};
    test::fake_server  silent{&sched, drop};
    test::fake_server  answering{&sched, test::answer_from(host_zone())};

    const auto resolver = native(config_for({silent.nameserver(), answering.nameserver()}, timeout));
    auto out = sched.run_to_completion(timed_resolve(&sched, &resolver, "host.test", {.proto = net::protocol::ipv4}));

    CHECK(out.result == std::vector<net::ip_addr>{addr4});
    CHECK(out.elapsed >= timeout);
    CHECK(silent.queries().size() == 1);
    CHECK(answering.queries().size() == 1);
}

TEST_CASE("a server whose port is closed is followed by the next at once", "[dns][native][retry]")
{
    net::io::scheduler sched{make_pool()};

    // a port nothing listens on: the OS refuses datagrams to it.
    std::uint16_t closed = 0;
    {
        auto sock = net::udp_socket::bind(&sched, net::ipv4_addr::loopback(), 0);
        REQUIRE(sock.has_value());
        closed = sock->local_port();
    }

    test::fake_server answering{&sched, test::answer_from(host_zone())};

    const auto resolver = native(config_for(
        {
            {.addr = net::ipv4_addr::loopback(), .port = closed},
            answering.nameserver()
    },
        10s));
    auto out = sched.run_to_completion(timed_resolve(&sched, &resolver, "host.test", {.proto = net::protocol::ipv4}));

    CHECK(out.result == std::vector<net::ip_addr>{addr4});
    CHECK(out.elapsed < 5s);
}

TEST_CASE("rotate starts each lookup at the next server", "[dns][native][retry]")
{
    const bool rotate = GENERATE(true, false);
    CAPTURE(rotate);

    const net::ipv4_addr first_addr{192, 0, 2, 1};
    const net::ipv4_addr second_addr{192, 0, 2, 2};

    net::io::scheduler sched{make_pool()};
    test::fake_server  first{
        &sched,
        test::answer_from({{"host.test", {test::a("host.test", first_addr)}}}
        )
    };
    test::fake_server second{
        &sched,
        test::answer_from({{"host.test", {test::a("host.test", second_addr)}}}
        )
    };

    auto conf   = config_for({first.nameserver(), second.nameserver()});
    conf.rotate = rotate;

    const auto resolver = native(conf);

    const auto test = [](net::io::scheduler* sched, const dns::resolver* resolver) -> task<std::vector<resolve_result>>
    {
        std::vector<resolve_result> results;
        results.reserve(3);
        for (int i = 0; i < 3; ++i)
            results.push_back(co_await resolver->resolve(sched, "host.test", {.proto = net::protocol::ipv4}));
        co_return results;
    };

    const auto results = sched.run_to_completion(test(&sched, &resolver));
    REQUIRE(results.size() == 3);

    using addrs = std::vector<net::ip_addr>;
    CHECK(results[0] == addrs{first_addr});
    CHECK(results[1] == addrs{rotate ? second_addr : first_addr});
    CHECK(results[2] == addrs{first_addr});
}

TEST_CASE("a server failure moves on to the next server", "[dns][native][errors]")
{
    const auto rcode = GENERATE(dns::rcode::server_failure, dns::rcode::refused, dns::rcode::not_implemented);
    CAPTURE(rcode);

    const auto failing = [rcode](const test::fake_query& q) -> std::vector<test::fake_reply>
    { return {{.msg = test::reply_to(q.msg, {}, rcode)}}; };

    net::io::scheduler sched{make_pool()};
    test::fake_server  bad{&sched, failing};
    test::fake_server  good{&sched, test::answer_from(host_zone())};

    const auto resolver = native(config_for({bad.nameserver(), good.nameserver()}));
    auto out = sched.run_to_completion(timed_resolve(&sched, &resolver, "host.test", {.proto = net::protocol::ipv4}));

    CHECK(out.result == std::vector<net::ip_addr>{addr4});
    CHECK(bad.queries().size() == 1);
    CHECK(good.queries().size() == 1);
}

TEST_CASE("answers' codes are mapped to the resolver's errors and conditions", "[dns][native][errors]")
{
    struct test_case
    {
        dns::rcode                    rcode = dns::rcode::no_error;
        dns::errc                     err   = dns::errc::server_failure;
        std::optional<dns::condition> cond{};
        std::size_t                   queries = 1; // from each server
    };

    auto tc = GENERATE(
        test_case{.rcode = dns::rcode::name_error, .err = dns::errc::name_not_found, .cond = dns::condition::not_found},
        // NOERROR without an address: the name exists
        test_case{.rcode = dns::rcode::no_error, .err = dns::errc::no_address, .cond = dns::condition::not_found},
        test_case{.rcode   = dns::rcode::server_failure,
                  .err     = dns::errc::server_failure,
                  .cond    = dns::condition::temporary_failure,
                  .queries = 2},
        test_case{.rcode = dns::rcode::refused, .err = dns::errc::refused, .queries = 2},
        test_case{.rcode   = dns::rcode::not_implemented,
                  .err     = dns::errc::server_failure,
                  .cond    = dns::condition::temporary_failure,
                  .queries = 2});
    CAPTURE(tc.rcode);

    const auto answer = [&tc](const test::fake_query& q) -> std::vector<test::fake_reply>
    { return {{.msg = test::reply_to(q.msg, {}, tc.rcode)}}; };

    net::io::scheduler sched{make_pool()};
    test::fake_server  server{&sched, answer};

    // two attempts: an answer that ends the lookup isn't asked again.
    const auto resolver = native(config_for({server.nameserver()}, 10s, 2));
    auto out = sched.run_to_completion(timed_resolve(&sched, &resolver, "host.test", {.proto = net::protocol::ipv4}));

    REQUIRE_FALSE(out.result.has_value());
    CHECK(out.result.error() == tc.err);
    CHECK(server.queries().size() == tc.queries);

    for (const auto cond : {dns::condition::not_found, dns::condition::temporary_failure})
    {
        CAPTURE(cond);
        CHECK((out.result.error() == cond) == (tc.cond == cond));
    }
}

TEST_CASE("servers that never answer are a server failure", "[dns][native][errors]")
{
    net::io::scheduler sched{make_pool()};
    test::fake_server  server{&sched, drop};

    const auto resolver = native(config_for({server.nameserver()}, 100ms, 2));
    auto out = sched.run_to_completion(timed_resolve(&sched, &resolver, "host.test", {.proto = net::protocol::ipv4}));

    REQUIRE_FALSE(out.result.has_value());
    CHECK(out.result.error() == dns::errc::server_failure);
    CHECK(out.result.error() == dns::condition::temporary_failure);
    CHECK(server.queries().size() == 2);
}

TEST_CASE("the search list is tried as ndots says", "[dns][native][search]")
{
    struct test_case
    {
        std::string              host{};
        int                      ndots = 1;
        std::vector<std::string> asked{};
        resolve_result           expected{std::unexpected(std::error_code{})};
    };

    const net::ipv4_addr search_addr{192, 0, 2, 5};
    const net::ipv4_addr dotted_addr{192, 0, 2, 6};

    using addrs = std::vector<net::ip_addr>;
    auto tc     = GENERATE_COPY(
        test_case{
            .host     = "host",
            .asked    = {"host.a.test", "host.b.test"},
            .expected = addrs{search_addr}
    },
        test_case{.host = "x.y", .asked = {"x.y"}, .expected = addrs{dotted_addr}},
        test_case{.host     = "x.y",
                  .ndots    = 2,
                  .asked    = {"x.y.a.test", "x.y.b.test", "x.y"},
                  .expected = addrs{dotted_addr}},
        test_case{.host     = "host.",
                  .asked    = {"host"},
                  .expected = std::unexpected(dns::make_error_code(dns::errc::name_not_found))},
        test_case{.host     = "nowhere",
                  .asked    = {"nowhere.a.test", "nowhere.b.test", "nowhere"},
                  .expected = std::unexpected(dns::make_error_code(dns::errc::name_not_found))},
        // the name as it was given has no address: its error wins over the search domains'
        test_case{.host     = "nodata",
                  .asked    = {"nodata.a.test", "nodata.b.test", "nodata"},
                  .expected = std::unexpected(dns::make_error_code(dns::errc::no_address))});
    CAPTURE(tc.host, tc.ndots);

    net::io::scheduler sched{make_pool()};
    test::fake_server  server{
        &sched,
        test::answer_from({
                           {"host.b.test", {test::a("host.b.test", search_addr)}},
                           {"x.y", {test::a("x.y", dotted_addr)}},
                           {"nodata", {test::aaaa("nodata", addr6)}},
                           }
        )
    };

    auto conf   = config_for({server.nameserver()});
    conf.search = {"a.test", "b.test"};
    conf.ndots  = tc.ndots;

    const auto resolver = native(conf);
    auto       out = sched.run_to_completion(timed_resolve(&sched, &resolver, tc.host, {.proto = net::protocol::ipv4}));
    CHECK(out.result == tc.expected);

    std::vector<std::string> asked;
    for (const auto& q : server.queries()) asked.push_back(q.msg.questions.front().name);
    CHECK(asked == tc.asked);
}

TEST_CASE("the hosts table is looked in as nsswitch.conf orders it", "[dns][native][hosts]")
{
    const net::ipv4_addr hosts_addr{192, 0, 2, 7};
    const net::ipv4_addr dns_addr{192, 0, 2, 8};

    struct test_case
    {
        dns::lookup_order order = dns::lookup_order::files_dns;
        std::string       host{};
        resolve_result    expected{std::unexpected(std::error_code{})};
        std::size_t       queries = 0;
    };

    using addrs = std::vector<net::ip_addr>;
    auto tc     = GENERATE_COPY(
        test_case{.order = dns::lookup_order::files_dns, .host = "both.test", .expected = addrs{hosts_addr}},
        test_case{.order = dns::lookup_order::files_dns, .host = "BOTH.test.", .expected = addrs{hosts_addr}},
        test_case{.order = dns::lookup_order::files_dns, .host = "dns.test", .expected = addrs{dns_addr}, .queries = 1},
        // the hosts table has only an IPv6 address for it: DNS is asked
        test_case{.order    = dns::lookup_order::files_dns,
                  .host     = "v6only.test",
                  .expected = std::unexpected(dns::make_error_code(dns::errc::name_not_found)),
                  .queries  = 1},
        test_case{.order    = dns::lookup_order::dns_files,
                  .host     = "both.test",
                  .expected = addrs{dns_addr},
                  .queries  = 1},
        test_case{.order    = dns::lookup_order::dns_files,
                  .host     = "hosts.test",
                  .expected = addrs{hosts_addr},
                  .queries  = 1},
        test_case{.order = dns::lookup_order::files, .host = "hosts.test", .expected = addrs{hosts_addr}},
        test_case{.order    = dns::lookup_order::files,
                  .host     = "dns.test",
                  .expected = std::unexpected(dns::make_error_code(dns::errc::name_not_found))},
        test_case{.order    = dns::lookup_order::files,
                  .host     = "v6only.test",
                  .expected = std::unexpected(dns::make_error_code(dns::errc::no_address))},
        test_case{.order    = dns::lookup_order::dns,
                  .host     = "hosts.test",
                  .expected = std::unexpected(dns::make_error_code(dns::errc::name_not_found)),
                  .queries  = 1});
    CAPTURE(tc.order, tc.host);

    net::io::scheduler sched{make_pool()};
    test::fake_server  server{
        &sched,
        test::answer_from({
                           {"both.test", {test::a("both.test", dns_addr)}},
                           {"dns.test", {test::a("dns.test", dns_addr)}},
                           }
        )
    };

    auto conf  = config_for({server.nameserver()});
    conf.order = tc.order;
    conf.hosts = {
        {  "both.test", {hosts_addr}},
        { "hosts.test", {hosts_addr}},
        {"v6only.test",      {addr6}},
    };

    const auto resolver = native(conf);
    auto       out = sched.run_to_completion(timed_resolve(&sched, &resolver, tc.host, {.proto = net::protocol::ipv4}));
    CHECK(out.result == tc.expected);
    CHECK(server.queries().size() == tc.queries);
}

TEST_CASE("CNAME chains are followed within the answer", "[dns][native][cname]")
{
    // a chain of links CNAMEs, from c0.test to c<links>.test, which has the address.
    const auto chain = [](int links)
    {
        test::zone z;
        for (int i = 0; i < links; ++i)
        {
            const auto name = "c" + std::to_string(i) + ".test";
            z[name]         = {test::cname(name, "c" + std::to_string(i + 1) + ".test")};
        }
        const auto last = "c" + std::to_string(links) + ".test";
        z[last]         = {test::a(last, addr4)};
        return z;
    };

    struct test_case
    {
        std::string    description{};
        test::zone     z{};
        resolve_result expected{std::unexpected(std::error_code{})};
    };

    using addrs = std::vector<net::ip_addr>;
    auto tc     = GENERATE_COPY(
        test_case{
            .description = "one link",
            .z           = chain(1),
            .expected    = addrs{addr4}
    },
        test_case{.description = "the most links", .z = chain(8), .expected = addrs{addr4}},
        test_case{.description = "a link too many",
                  .z           = chain(9),
                  .expected    = std::unexpected(dns::make_error_code(dns::errc::bad_response))},
        test_case{
            .description = "a loop",
            .z = {{"c0.test", {test::cname("c0.test", "c1.test")}}, {"c1.test", {test::cname("c1.test", "C0.test")}}},
            .expected = std::unexpected(dns::make_error_code(dns::errc::bad_response))},
        // the chain's end isn't in the answer: the name exists, without an address
        test_case{.description = "a chain cut short",
                  .z           = {{"c0.test", {test::cname("c0.test", "c1.test")}}},
                  .expected    = std::unexpected(dns::make_error_code(dns::errc::no_address))});
    CAPTURE(tc.description);

    net::io::scheduler sched{make_pool()};
    test::fake_server  server{&sched, test::answer_from(tc.z)};

    const auto resolver = native(config_for({server.nameserver()}));
    auto out = sched.run_to_completion(timed_resolve(&sched, &resolver, "c0.test", {.proto = net::protocol::ipv4}));
    CHECK(out.result == tc.expected);
}

TEST_CASE("a nameserver on IPv6 is asked", "[dns][native][udp]")
{
    net::io::scheduler sched{make_pool()};
    if (!net::udp_socket::bind(&sched, net::ipv6_addr::loopback(), 0).has_value()) SKIP("no IPv6 loopback");

    test::fake_server server{&sched, test::answer_from(host_zone()), net::ipv6_addr::loopback()};

    const auto resolver = native(config_for({server.nameserver()}));
    auto       out      = sched.run_to_completion(timed_resolve(&sched, &resolver, "host.test"));

    REQUIRE(out.result.has_value());
    CHECK(set_of(*out.result) == set_of({addr4, addr6}));
}

TEST_CASE("a native lookup ends at the caller's deadline, or when stopped", "[dns][native][deadline][stop]")
{
    net::io::scheduler sched{make_pool()};
    test::fake_server  server{&sched, drop};

    const auto resolver = native(config_for({server.nameserver()}, 10s, 2));

    SECTION("the deadline")
    {
        // (timed from before the deadline's taken, so it can't seem to end early.)
        const auto test = [](net::io::scheduler* sched, const dns::resolver* resolver) -> task<timed>
        {
            const auto start  = net::io::clock::now();
            auto       result = co_await resolver->resolve(sched, "host.test", {.deadline = start + 100ms});
            co_return timed{.result = std::move(result), .elapsed = net::io::clock::now() - start};
        };

        const auto out = sched.run_to_completion(test(&sched, &resolver));
        REQUIRE_FALSE(out.result.has_value());
        CHECK(out.result.error() == std::errc::timed_out);
        CHECK(out.elapsed >= 100ms);
        CHECK(out.elapsed < 5s);
    }

    SECTION("stop")
    {
        const auto test =
            [](net::io::scheduler* sched, const dns::resolver* resolver, const test::fake_server* server) -> task<timed>
        {
            std::stop_source source;
            if (!sched->spawn(
                    [](net::io::scheduler* sched, const test::fake_server* server, std::stop_source source) -> task<>
                    {
                        if (co_await wait_received(sched, server, 1)) source.request_stop();
                    }(sched, server, source)))
                co_return timed{.failed = "spawn failed"};

            co_return co_await timed_resolve(sched,
                                             resolver,
                                             "host.test",
                                             {.deadline = net::io::clock::now() + 10s, .stop = source.get_token()});
        };

        const auto out = sched.run_to_completion(test(&sched, &resolver, &server));
        CAPTURE(out.failed);
        REQUIRE(out.failed.empty());
        REQUIRE_FALSE(out.result.has_value());
        CHECK(out.result.error() == std::errc::operation_canceled);
        CHECK(out.elapsed < 5s);
    }

    SECTION("the scheduler stopping")
    {
        const auto test =
            [](net::io::scheduler* sched, const dns::resolver* resolver, const test::fake_server* server) -> task<timed>
        {
            if (!sched->spawn(
                    [](net::io::scheduler* sched, const test::fake_server* server) -> task<>
                    {
                        if (co_await wait_received(sched, server, 1)) sched->request_stop();
                    }(sched, server)))
                co_return timed{.failed = "spawn failed"};

            co_return co_await timed_resolve(sched, resolver, "host.test", {.deadline = net::io::clock::now() + 10s});
        };

        const auto out = sched.run_to_completion(test(&sched, &resolver, &server));
        CAPTURE(out.failed);
        REQUIRE(out.failed.empty());
        REQUIRE_FALSE(out.result.has_value());
        CHECK(out.result.error() == std::errc::operation_canceled);
        CHECK(out.elapsed < 5s);
    }
}

TEST_CASE("a native lookup whose callers have all left asks no more", "[dns][native][stop]")
{
    static constexpr auto timeout = 200ms;

    net::io::scheduler sched{make_pool()};
    test::fake_server  server{&sched, drop};

    // three attempts, a timeout apart, if it went on.
    const auto resolver = native(config_for({server.nameserver()}, timeout, 3));

    const auto test =
        [](net::io::scheduler* sched, const dns::resolver* resolver, const test::fake_server* server) -> task<timed>
    {
        // the caller leaves once the first query's been received (before that, the lookup would be stopped before it
        // asked anything).
        std::stop_source source;
        if (!sched->spawn(
                [](net::io::scheduler* sched, const test::fake_server* server, std::stop_source source) -> task<>
                {
                    if (co_await wait_received(sched, server, 1)) source.request_stop();
                }(sched, server, source)))
            co_return timed{.failed = "spawn failed"};

        auto result = co_await resolver->resolve(
            sched,
            "host.test",
            {.proto = net::protocol::ipv4, .deadline = net::io::clock::now() + 10s, .stop = source.get_token()});

        // long enough for every attempt (from the first, which was before the caller left). NOTE: timed, as nothing
        // would say that an attempt is still to come.
        (void)co_await sched->sleep_until(net::io::clock::now() + (4 * timeout));
        co_return timed{.result = std::move(result)};
    };

    const auto out = sched.run_to_completion(test(&sched, &resolver, &server));
    CAPTURE(out.failed);
    REQUIRE(out.failed.empty());
    REQUIRE_FALSE(out.result.has_value());
    CHECK(out.result.error() == std::errc::operation_canceled);
    CHECK(server.queries().size() == 1);
}

TEST_CASE("concurrent native lookups of one name share one", "[dns][native][merge]")
{
    constexpr std::size_t callers = 4;

    // a pool of one worker: the callers all join before the lookup's task runs (see resolver.cpp's tests). The answer
    // is a little late too.
    const auto late = [](const test::fake_query& q)
    {
        auto replies = test::answer_from(host_zone())(q);
        for (auto& r : replies) r.delay = 50ms;
        return replies;
    };

    // what the callers write to, which outlives the scheduler's tasks.
    std::vector<resolve_result> results(callers, std::unexpected(std::error_code{}));
    std::atomic<std::uint64_t>  returned{0};

    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(1)};
    test::fake_server  server{&sched, late};

    const auto resolver = native(config_for({server.nameserver()}));

    const auto test = [](net::io::scheduler*          sched,
                         const dns::resolver*         resolver,
                         std::vector<resolve_result>* results,
                         std::atomic<std::uint64_t>*  returned) -> task<bool>
    {
        for (auto& result : *results)
        {
            const bool spawned = sched->spawn(
                [](net::io::scheduler*         sched,
                   const dns::resolver*        resolver,
                   resolve_result*             result,
                   std::atomic<std::uint64_t>* returned) -> task<>
                {
                    *result = co_await resolver->resolve(sched, "host.test");
                    returned->fetch_add(1);
                    sched->notify_all(*returned);
                }(sched, resolver, &result, returned));
            if (!spawned) co_return false;
        }

        const auto until = net::io::clock::now() + 10s;
        for (auto seen = returned->load(); seen < results->size(); seen = returned->load())
        {
            if (auto err = co_await sched->wait_on(*returned, seen, until); err) co_return false;
        }
        co_return true;
    };

    REQUIRE(sched.run_to_completion(test(&sched, &resolver, &results, &returned)));

    for (const auto& result : results)
    {
        REQUIRE(result.has_value());
        CHECK(set_of(*result) == set_of({addr4, addr6}));
    }

    // one A and one AAAA, for all of them.
    CHECK(server.queries().size() == 2);
}

TEST_CASE("native lookups finish whichever way their callers leave", "[dns][native][merge][stop]")
{
    // the answer is a little late, by a different amount each time, while callers leave around it: by their deadline,
    // by stop, or not at all. (Run under ThreadSanitizer in CI, for the races between them.)
    constexpr std::size_t rounds = 20;

    struct round_result
    {
        resolve_result staying{std::unexpected(std::error_code{})};
        resolve_result timing_out{std::unexpected(std::error_code{})};
        resolve_result stopped{std::unexpected(std::error_code{})};
    };

    // what the callers write to, which outlives the scheduler's tasks.
    std::vector<round_result>  results(rounds);
    std::atomic<std::uint64_t> returned{0};

    std::atomic<int> answered{0};
    const auto       late = [&answered](const test::fake_query& q)
    {
        auto replies = test::answer_from(host_zone())(q);
        for (auto& r : replies) r.delay = std::chrono::milliseconds{answered.fetch_add(1) % 4};
        return replies;
    };

    net::io::scheduler sched{make_pool()};
    test::fake_server  server{&sched, late};

    const auto resolver = native(config_for({server.nameserver()}));

    const auto call = [](net::io::scheduler*         sched,
                         const dns::resolver*        resolver,
                         dns::resolve_options        options,
                         resolve_result*             result,
                         std::atomic<std::uint64_t>* returned) -> task<>
    {
        *result = co_await resolver->resolve(sched, "host.test", std::move(options));
        returned->fetch_add(1);
        sched->notify_all(*returned);
    };

    const auto test = [&call](net::io::scheduler*         sched,
                              const dns::resolver*        resolver,
                              std::vector<round_result>*  results,
                              std::atomic<std::uint64_t>* returned) -> task<bool>
    {
        for (std::size_t i = 0; i < results->size(); ++i)
        {
            auto&            out = (*results)[i];
            std::stop_source stop;

            const auto deadline = net::io::clock::now() + std::chrono::milliseconds{i % 3};
            if (!sched->spawn(call(sched, resolver, {}, &out.staying, returned))
                || !sched->spawn(call(sched, resolver, {.deadline = deadline}, &out.timing_out, returned))
                || !sched->spawn(call(sched, resolver, {.stop = stop.get_token()}, &out.stopped, returned)))
                co_return false;

            (void)co_await sched->sleep_until(net::io::clock::now() + std::chrono::milliseconds{i % 2});
            stop.request_stop();

            const auto until = net::io::clock::now() + 10s;
            for (auto seen = returned->load(); seen < 3 * (i + 1); seen = returned->load())
            {
                if (auto err = co_await sched->wait_on(*returned, seen, until); err) co_return false;
            }
        }
        co_return true;
    };

    REQUIRE(sched.run_to_completion(test(&sched, &resolver, &results, &returned)));

    const auto answer_or = [](const resolve_result& result, std::errc err)
    {
        if (result.has_value()) return set_of(*result) == set_of({addr4, addr6});
        return result.error() == err;
    };

    for (std::size_t i = 0; i < results.size(); ++i)
    {
        CAPTURE(i);
        REQUIRE(results[i].staying.has_value());
        CHECK(set_of(*results[i].staying) == set_of({addr4, addr6}));
        CHECK(answer_or(results[i].timing_out, std::errc::timed_out));
        CHECK(answer_or(results[i].stopped, std::errc::operation_canceled));
    }
}

// The native back end against the machine's own configuration (/etc's files, not one the test gives it) and real
// nameservers, as the system back end's [network] tests in resolver.cpp. (CI's back end is the system's on its Linux
// and on macOS, but automatic picks the native one on e.g. Debian's and Ubuntu's servers.)

TEST_CASE("a native lookup tells of a family's addresses as soon as they're in", "[dns][native][resolution]")
{
    // one family's answer is late (sent apart, so the other's isn't held up behind it).
    constexpr auto late = 300ms;

    const auto late_type = GENERATE(dns::type::a, dns::type::aaaa);
    CAPTURE(late_type == dns::type::a ? "A" : "AAAA");

    const auto handler = [late_type, late](const test::fake_query& q)
    {
        auto replies = test::answer_from(host_zone())(q);
        if (q.msg.questions.front().type == late_type)
        {
            for (auto& r : replies)
            {
                r.delay = late;
                r.apart = true;
            }
        }
        return replies;
    };

    // what the resolution had found: once the first family's in, and once both are; and when.
    struct seen
    {
        dns::detail::found         first{};
        net::io::clock::time_point first_when{};
        dns::detail::found         done{};
        net::io::clock::time_point done_when{};
        std::string                failed{}; // what went wrong in the test's own steps
    };

    net::io::scheduler sched{make_pool()};
    test::fake_server  server{&sched, handler};

    const auto resolver = native(config_for({server.nameserver()}));

    const auto test = [](net::io::scheduler* sched, const dns::resolver* resolver) -> task<seen>
    {
        auto res = resolver->start_resolving(sched, "host.test", net::protocol::not_care);
        if (!res.has_value()) co_return seen{.failed = "start_resolving: " + res.error().message()};

        const auto wait = [](net::io::scheduler*            sched,
                             const dns::detail::resolution* res,
                             std::uint64_t                  version) -> task<dns::detail::found>
        {
            const auto until = net::io::clock::now() + 10s;
            while (res->news().load() == version)
            {
                if (auto err = co_await sched->wait_on(res->news(), version, until); err) break;
            }
            co_return res->look();
        };

        seen out;
        out.first      = co_await wait(sched, &*res, 0);
        out.first_when = net::io::clock::now();
        out.done       = co_await wait(sched, &*res, out.first.version);
        out.done_when  = net::io::clock::now();
        co_return out;
    };

    const auto out = sched.run_to_completion(test(&sched, &resolver));
    CAPTURE(out.failed);
    REQUIRE(out.failed.empty());

    const auto early = late_type == dns::type::a ? net::protocol::ipv6 : net::protocol::ipv4;
    REQUIRE_FALSE(out.first.done);
    CHECK(out.first.family == early);
    CHECK(out.first.addrs
          == std::vector<net::ip_addr>{early == net::protocol::ipv4 ? net::ip_addr{addr4} : net::ip_addr{addr6}});

    CHECK(out.done.done);
    CHECK(set_of(out.done.addrs) == set_of({addr4, addr6}));
    CHECK(out.done_when - out.first.at >= late / 2);

    // asked once each, at once.
    CHECK(server.queries().size() == 2);
}

TEST_CASE("a native lookup of one family tells of nothing until it's done", "[dns][native][resolution]")
{
    net::io::scheduler sched{make_pool()};
    test::fake_server  server{&sched, test::answer_from(host_zone())};

    const auto resolver = native(config_for({server.nameserver()}));

    const auto test = [](net::io::scheduler*  sched,
                         const dns::resolver* resolver) -> task<std::expected<dns::detail::found, std::error_code>>
    {
        auto res = resolver->start_resolving(sched, "host.test", net::protocol::ipv4);
        if (!res.has_value()) co_return std::unexpected(res.error());

        const auto until = net::io::clock::now() + 10s;
        while (res->news().load() == 0)
        {
            if (auto err = co_await sched->wait_on(res->news(), 0, until); err) break;
        }
        co_return res->look();
    };

    const auto looked = sched.run_to_completion(test(&sched, &resolver));
    CAPTURE(looked.error_or({}).message());
    REQUIRE(looked.has_value());
    const auto& found = *looked;
    CHECK(found.done);
    CHECK(found.version == 1);
    CHECK(found.addrs == std::vector<net::ip_addr>{addr4});
}

TEST_CASE("the native resolver finds a public name", "[dns][native][.network]")
{
    struct test_case
    {
        net::protocol proto = net::protocol::not_care;
    };

    // asking for AAAA records doesn't need the machine to have IPv6 (only sorting them does, which mustn't fail
    // without it).
    auto tc = GENERATE(test_case{.proto = net::protocol::not_care},
                       test_case{.proto = net::protocol::ipv4},
                       test_case{.proto = net::protocol::ipv6});
    CAPTURE(tc.proto);

    const dns::resolver resolver{{.use = dns::resolver::backend::native}};
    net::io::scheduler  sched{make_pool()};

    auto out = sched.run_to_completion(timed_resolve(&sched, &resolver, "google.com", {.proto = tc.proto}));
    CAPTURE(out.result.has_value() ? std::string{} : out.result.error().message());
    REQUIRE(out.result.has_value());
    CHECK_FALSE(out.result->empty());

    for (const auto& addr : *out.result)
    {
        CAPTURE(addr.to_string());
        if (tc.proto == net::protocol::ipv4) CHECK(addr.is_ipv4());
        if (tc.proto == net::protocol::ipv6) CHECK(addr.is_ipv6());
        if (addr.is_ipv4()) CHECK_FALSE(addr.to_ipv4().is_loopback());
        if (addr.is_ipv6()) CHECK_FALSE(addr.to_ipv6().is_loopback());
    }

    // both families' records, whichever order RFC 6724 puts them in.
    if (tc.proto == net::protocol::not_care)
    {
        CHECK(std::ranges::any_of(*out.result, [](const net::ip_addr& addr) { return addr.is_ipv4(); }));
        CHECK(std::ranges::any_of(*out.result, [](const net::ip_addr& addr) { return addr.is_ipv6(); }));
    }
}

TEST_CASE("the native resolver reports names that don't exist", "[dns][native][.network]")
{
    const dns::resolver resolver{{.use = dns::resolver::backend::native}};
    net::io::scheduler  sched{make_pool()};

    // .invalid never resolves (RFC 6761), nor does it under a search domain.
    auto out = sched.run_to_completion(timed_resolve(&sched, &resolver, "nothing.invalid"));
    REQUIRE_FALSE(out.result.has_value());
    CAPTURE(out.result.error().message());
    CHECK(out.result.error() == dns::condition::not_found);
}

TEST_CASE("the native resolver follows a public name's CNAME chain", "[dns][native][cname][.network]")
{
    // names that have been CNAMEs for years: one link (to github.com, which has no AAAA records: that family's
    // question finds no address, and the other's answer stands), and several (through Akamai's names). Were one to stop
    // being a CNAME, it would still resolve, testing less.
    auto host = GENERATE(as<std::string>{}, "www.github.com", "www.microsoft.com");
    CAPTURE(host);

    const dns::resolver resolver{{.use = dns::resolver::backend::native}};
    net::io::scheduler  sched{make_pool()};

    auto out = sched.run_to_completion(timed_resolve(&sched, &resolver, host));
    CAPTURE(out.result.has_value() ? std::string{} : out.result.error().message());
    REQUIRE(out.result.has_value());
    CHECK_FALSE(out.result->empty());
    CHECK(std::ranges::any_of(*out.result, [](const net::ip_addr& addr) { return addr.is_ipv4(); }));
}
