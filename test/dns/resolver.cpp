#include "dns/resolver.hpp"

#include <atomic>
#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <exception> // IWYU pragma: keep
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <netdb.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "coro/task.hpp"
#include "coro/thread_pool.hpp"
#include "dns/blocking_lookup.hpp"
#include "dns/native.hpp"
#include "exception.hpp"
#include "io/io.hpp"
#include "io/scheduler.hpp"
#include "ip_addr.hpp"
#include "ipv4_addr.hpp"
#include "ipv6_addr.hpp"
#include "util/defer.hpp"

namespace
{

using namespace std::chrono_literals;

using net::coro::task;
using resolve_result = std::expected<std::vector<net::ip_addr>, std::error_code>;

std::shared_ptr<net::coro::thread_pool> make_pool() { return std::make_shared<net::coro::thread_pool>(2); }

// test_ipv6 is an IPv6 address (the documentation's) that RFC 6724 sorts after 127.0.0.1 on any machine: if it has a
// route to it, by its label (or precedence), and otherwise as it can't be reached.
const net::ip_addr test_ipv6{
    net::ipv6_addr{0x2001, 0xdb8, 0, 0, 0, 0, 0, 1}
};

// wait_started waits (in a task) until lookup has started n lookups, returning whether it has: not if the scheduler's
// stopped, or they haven't within 10 seconds.
task<bool> wait_started(net::io::scheduler* sched, const test::blocking_lookup* lookup, int n)
{
    const auto until = net::io::clock::now() + 10s;
    while (lookup->started.load() < n)
    {
        if (net::io::clock::now() >= until) co_return false;
        if (auto err = co_await sched->sleep_until(net::io::clock::now() + 1ms); err) co_return false;
    }
    co_return true;
}

// one_worker is a pool of one thread, which runs one task at a time: a caller joins (or starts) a lookup without
// suspending, so a task that runs after a caller has called resolve() (e.g. one that saw caller_counts::called count
// it) runs after it has joined.
std::shared_ptr<net::coro::thread_pool> one_worker() { return std::make_shared<net::coro::thread_pool>(1); }

// caller is one of a test's concurrent resolve() calls: what it asks for, and what it got.
struct caller
{
    std::string                               host{};
    net::protocol                             proto = net::protocol::not_care;
    std::optional<net::io::clock::time_point> deadline{};
    std::stop_token                           stop{};
    resolve_result                            result{std::unexpected(std::error_code{})};
};

// caller_counts counts a test's callers: those that have called resolve(), and those that it has returned to.
struct caller_counts
{
    std::atomic<std::uint64_t> called{0};
    std::atomic<std::uint64_t> returned{0};
};

// call makes c's resolve() call, counting it in counts.
task<> call(net::io::scheduler* sched, const net::dns::resolver* resolver, caller* c, caller_counts* counts)
{
    counts->called.fetch_add(1);
    sched->notify_all(counts->called);

    c->result =
        co_await resolver->resolve(sched, c->host, {.proto = c->proto, .deadline = c->deadline, .stop = c->stop});

    counts->returned.fetch_add(1);
    sched->notify_all(counts->returned);
}

// start spawns callers' calls, returning whether the scheduler took them all.
[[nodiscard]] bool
start(net::io::scheduler* sched, const net::dns::resolver* resolver, std::span<caller> callers, caller_counts* counts)
{
    for (auto& c : callers)
    {
        if (!sched->spawn(call(sched, resolver, &c, counts))) return false;
    }
    return true;
}

// wait_count waits (in a task) until count reaches n, returning whether it has: not if the scheduler's stopped, or it
// hasn't within 10 seconds.
task<bool> wait_count(net::io::scheduler* sched, const std::atomic<std::uint64_t>* count, std::uint64_t n)
{
    const auto until = net::io::clock::now() + 10s;
    for (auto seen = count->load(); seen < n; seen = count->load())
    {
        if (auto err = co_await sched->wait_on(*count, seen, until); err) co_return false;
    }
    co_return true;
}

// timed is a resolution's result, and how long it took.
struct timed
{
    resolve_result           result{std::unexpected(std::error_code{})};
    net::io::clock::duration elapsed{};
    int                      finished_then = 0; // how many lookups had finished when it returned

    // what went wrong in the test's own steps, if anything (for the test thread to check: Catch2's assertions aren't
    // for the scheduler's threads).
    std::string failed{};
};

}

TEST_CASE("the resolver's errors compare with its conditions", "[dns][resolver][errors]")
{
    using net::dns::condition;
    using net::dns::errc;

    CHECK(std::error_code{errc::name_not_found} == condition::not_found);
    CHECK(std::error_code{errc::no_address} == condition::not_found);
    CHECK(std::error_code{errc::server_failure} == condition::temporary_failure);
    CHECK(std::error_code{errc::name_not_found} != condition::temporary_failure);
    CHECK(std::error_code{errc::refused} != condition::not_found);
    CHECK(std::error_code{errc::refused} != condition::temporary_failure);
    CHECK(std::error_code{errc::bad_response} != condition::not_found);
    CHECK(std::error_code{errc::bad_response} != condition::temporary_failure);

    // getaddrinfo()'s codes too.
    CHECK(net::make_gai_error_code(EAI_NONAME) == condition::not_found);
#ifdef EAI_NODATA
    CHECK(net::make_gai_error_code(EAI_NODATA) == condition::not_found);
#endif
    CHECK(net::make_gai_error_code(EAI_AGAIN) == condition::temporary_failure);
    CHECK(net::make_gai_error_code(EAI_FAIL) == condition::temporary_failure);
    CHECK(net::make_gai_error_code(EAI_NONAME) != condition::temporary_failure);
    CHECK(net::make_gai_error_code(EAI_AGAIN) != condition::not_found);
    CHECK(net::make_gai_error_code(EAI_MEMORY) != condition::not_found);
    CHECK(net::make_gai_error_code(EAI_MEMORY) != condition::temporary_failure);

    // and nothing else.
    CHECK(std::make_error_code(std::errc::timed_out) != condition::temporary_failure);
    CHECK(std::make_error_code(std::errc::invalid_argument) != condition::not_found);

    CHECK(std::error_code{errc::name_not_found}.category() == net::dns::category());
    CHECK(std::error_code{errc::name_not_found}.message() == "name not found");
    CHECK(std::error_condition{condition::not_found}.message() == "not found");
}

TEST_CASE("the resolver returns IP addresses without a lookup", "[dns][resolver][literal]")
{
    test::blocking_lookup lookup;

    // one thread for blocking calls, which a lookup holds: an address doesn't need it.
    net::io::scheduler sched{make_pool(), {.blocking_threads = 1}};
    net::util::defer   release{[&lookup] noexcept { lookup.release(); }};

    REQUIRE(sched.spawn([](net::io::scheduler* sched, test::blocking_lookup* lookup) -> task<>
                        { (void)co_await lookup->resolver.resolve(sched, "held.test"); }(&sched, &lookup)));
    REQUIRE(lookup.wait_started(1));

    struct test_case
    {
        std::string               host{};
        net::protocol             proto = net::protocol::not_care;
        std::vector<net::ip_addr> expected{};
        std::error_code           err{};
    };

    auto tc = GENERATE(test_case{.host = "127.0.0.1", .expected = {net::ipv4_addr::loopback()}},
                       test_case{.host = "::1", .expected = {net::ipv6_addr::loopback()}},
                       test_case{.host = "::1", .proto = net::protocol::ipv6, .expected = {net::ipv6_addr::loopback()}},
                       test_case{.host  = "::1",
                                 .proto = net::protocol::ipv4,
                                 .err   = std::make_error_code(std::errc::address_family_not_supported)},
                       test_case{.host  = "127.0.0.1",
                                 .proto = net::protocol::ipv6,
                                 .err   = std::make_error_code(std::errc::address_family_not_supported)});
    CAPTURE(tc.host, tc.proto);

    // the system's resolver, and a test's, alike.
    for (const auto* resolver : {&net::dns::default_resolver(), &lookup.resolver})
    {
        auto resolving = resolver->resolve(&sched, tc.host, {.proto = tc.proto});
        REQUIRE(resolving.finished_without_coroutine());

        auto result = std::move(resolving).result();
        if (tc.err)
        {
            REQUIRE_FALSE(result.has_value());
            CHECK(result.error() == tc.err);
        }
        else
        {
            REQUIRE(result.has_value());
            CHECK(*result == tc.expected);
        }
    }

    CHECK(lookup.started.load() == 1);
}

TEST_CASE("the resolver checks names before looking them up", "[dns][resolver][names]")
{
    std::atomic<int>         looked_up{0};
    const net::dns::resolver resolver{
        {
         .use           = net::dns::resolver::backend::system,
         .system_lookup = [&looked_up](const std::string& /*host*/, net::protocol /*proto*/) -> resolve_result
            {
                looked_up.fetch_add(1);
                return std::vector<net::ip_addr>{net::ipv4_addr::loopback()};
            }, }
    };

    net::io::scheduler sched{make_pool()};

    const std::string label63(63, 'a');

    struct test_case
    {
        std::string host{};
        bool        valid = false;
    };

    auto tc = GENERATE_COPY(
        test_case{.host = "localhost", .valid = true},
        test_case{.host = "example.com", .valid = true},
        test_case{.host = "example.com.", .valid = true},
        test_case{.host = "_service._tcp.example.com", .valid = true},
        test_case{.host = "a-b.c-d.example", .valid = true},
        test_case{.host = "UPPER.example", .valid = true},
        test_case{.host = "127.1", .valid = true}, // getaddrinfo() reads it as an address
        test_case{.host = label63 + ".example", .valid = true},
        test_case{.host = label63 + "a.example"},
        // 253 bytes (4 labels of 63, and 3 dots, is 255), then 254.
        test_case{.host = label63 + "." + label63 + "." + label63 + "." + std::string(61, 'a'), .valid = true},
        test_case{.host = label63 + "." + label63 + "." + label63 + "." + std::string(61, 'a') + ".", .valid = true},
        test_case{.host = label63 + "." + label63 + "." + label63 + "." + std::string(62, 'a')},
        test_case{.host = ""},
        test_case{.host = "."},
        test_case{.host = "a..b"},
        test_case{.host = ".a"},
        test_case{.host = "-a.example"},
        test_case{.host = "a-.example"},
        test_case{.host = "a.example-"},
        test_case{.host = "a b.example"},
        test_case{.host = "a/b.example"},
        test_case{.host = "caf\xc3\xa9.example"},
        test_case{.host = "fe80::1%en0"}, // a zone, which ip_addr doesn't have
        test_case{.host = "[::1]"});
    CAPTURE(tc.host);

    auto result = sched.run_to_completion(resolver.resolve(&sched, tc.host));
    if (tc.valid)
    {
        CHECK(result.has_value());
        CHECK(looked_up.load() == 2); // a call for each family
    }
    else
    {
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == std::errc::invalid_argument);
        CHECK(looked_up.load() == 0);
    }
}

TEST_CASE("the resolver returns the lookup's result", "[dns][resolver]")
{
    struct test_case
    {
        resolve_result answer{std::unexpected(std::error_code{})};
    };

    auto tc = GENERATE(
        test_case{
            .answer = std::vector<net::ip_addr>{net::ipv6_addr::loopback(), net::ipv4_addr::loopback()}
    },
        test_case{.answer = std::unexpected(net::make_gai_error_code(EAI_NONAME))},
        test_case{.answer = std::unexpected(net::make_gai_error_code(EAI_AGAIN))});

    std::string              asked_host;
    net::protocol            asked_proto = net::protocol::not_care;
    const net::dns::resolver resolver{
        {
         .use           = net::dns::resolver::backend::system,
         .system_lookup = [&](const std::string& host, net::protocol proto) -> resolve_result
            {
                asked_host  = host;
                asked_proto = proto;
                return tc.answer;
            }, }
    };

    net::io::scheduler sched{make_pool()};

    auto result = sched.run_to_completion(resolver.resolve(&sched, "example.test", {.proto = net::protocol::ipv6}));
    CHECK(result == tc.answer);
    CHECK(asked_host == "example.test");
    CHECK(asked_proto == net::protocol::ipv6);

    if (!tc.answer.has_value() && tc.answer.error() == net::make_gai_error_code(EAI_NONAME))
    {
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == net::dns::condition::not_found);
    }
}

TEST_CASE("a lookup that outlives its resolver still finishes", "[dns][resolver]")
{
    net::io::scheduler sched{make_pool()};

    auto resolving = [&sched]
    {
        const net::dns::resolver resolver{
            {
             .use           = net::dns::resolver::backend::system,
             .system_lookup = [](const std::string& /*host*/, net::protocol /*proto*/) -> resolve_result
                { return std::vector<net::ip_addr>{net::ipv4_addr::loopback()}; },
             }
        };
        return resolver.resolve(&sched, "example.test");
    }();

    auto result = sched.run_to_completion(std::move(resolving));
    REQUIRE(result.has_value());
    CHECK(*result == std::vector<net::ip_addr>{net::ipv4_addr::loopback()});
}

TEST_CASE("a caller gives up on a lookup at its deadline, while the lookup goes on", "[dns][resolver][deadline]")
{
    test::blocking_lookup lookup;

    net::io::scheduler sched{make_pool()};
    net::util::defer   release{[&lookup] noexcept { lookup.release(); }};

    const auto test = [](net::io::scheduler* sched, test::blocking_lookup* lookup) -> task<timed>
    {
        // lets its calls go however this ends (the scheduler waits for them).
        const net::util::defer let_go{[lookup] noexcept { lookup->release(); }};

        // a share of the lookup, held until the caller has given up, so that both families' calls have been given
        // threads before its deadline can pass: a lookup whose callers have all left before then never makes them.
        auto held = lookup->resolver.start_resolving(sched, "slow.test", net::protocol::not_care);
        if (!held.has_value()) co_return timed{.failed = "start_resolving: " + held.error().message()};
        if (!co_await wait_started(sched, lookup, 2)) co_return timed{.failed = "the lookup's calls didn't start"};

        const auto start  = net::io::clock::now();
        auto       result = co_await lookup->resolver.resolve(sched, "slow.test", {.deadline = start + 100ms});
        timed      out{.result = std::move(result), .elapsed = net::io::clock::now() - start};

        out.finished_then = lookup->finished.load();
        lookup->release(); // its result, once it has one, is dropped.
        co_return out;
    };

    const auto out = sched.run_to_completion(test(&sched, &lookup));

    CAPTURE(out.failed);
    REQUIRE(out.failed.empty());
    REQUIRE_FALSE(out.result.has_value());
    CHECK(out.result.error() == std::errc::timed_out);
    CHECK(out.result.error() == net::io::status_condition::timed_out);
    CHECK(out.elapsed >= 100ms);
    CHECK(out.elapsed < 5s);
    CHECK(lookup.started.load() == 2); // a call for each family
    CHECK(out.finished_then == 0);

    // run_to_completion() waited for the lookup to finish.
    CHECK(lookup.finished.load() == 2);
}

TEST_CASE("a caller gives up on a lookup when stop is requested", "[dns][resolver][stop]")
{
    test::blocking_lookup lookup;

    net::io::scheduler sched{make_pool()};
    net::util::defer   release{[&lookup] noexcept { lookup.release(); }};

    SECTION("while the lookup runs")
    {
        const auto test = [](net::io::scheduler* sched, test::blocking_lookup* lookup) -> task<timed>
        {
            std::stop_source source;

            // stops once the lookup has started (both families' calls).
            if (!sched->spawn(
                    [](net::io::scheduler* sched, test::blocking_lookup* lookup, std::stop_source source) -> task<>
                    {
                        if (co_await wait_started(sched, lookup, 2)) source.request_stop();
                    }(sched, lookup, source)))
                co_return timed{.failed = "spawn failed"};

            const auto start = net::io::clock::now();
            auto  result     = co_await lookup->resolver.resolve(sched,
                                                                 "slow.test",
                                                                 {.deadline = start + 10s, .stop = source.get_token()});
            timed out{.result = std::move(result), .elapsed = net::io::clock::now() - start};

            out.finished_then = lookup->finished.load();
            lookup->release();
            co_return out;
        };

        const auto out = sched.run_to_completion(test(&sched, &lookup));

        CAPTURE(out.failed);
        REQUIRE(out.failed.empty());
        REQUIRE_FALSE(out.result.has_value());
        CHECK(out.result.error() == std::errc::operation_canceled);
        CHECK(out.elapsed < 5s);
        CHECK(out.finished_then == 0);
        CHECK(lookup.started.load() == 2);
        CHECK(lookup.finished.load() == 2);
    }

    SECTION("before it's called")
    {
        std::stop_source source;
        source.request_stop();

        auto resolving = lookup.resolver.resolve(&sched, "slow.test", {.stop = source.get_token()});
        REQUIRE(resolving.finished_without_coroutine());

        auto result = std::move(resolving).result();
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == std::errc::operation_canceled);
        CHECK(lookup.started.load() == 0);
    }
}

TEST_CASE("stopping the scheduler ends the lookups' waits", "[dns][resolver][stop]")
{
    test::blocking_lookup lookup;

    net::io::scheduler sched{make_pool()};
    net::util::defer   release{[&lookup] noexcept { lookup.release(); }};

    const auto test = [](net::io::scheduler* sched, test::blocking_lookup* lookup) -> task<timed>
    {
        if (!sched->spawn(
                [](net::io::scheduler* sched, test::blocking_lookup* lookup) -> task<>
                {
                    if (co_await wait_started(sched, lookup, 2)) sched->request_stop();
                }(sched, lookup)))
            co_return timed{.failed = "spawn failed"};

        const auto start  = net::io::clock::now();
        auto       result = co_await lookup->resolver.resolve(sched, "slow.test", {.deadline = start + 10s});
        timed      out{.result = std::move(result), .elapsed = net::io::clock::now() - start};

        out.finished_then = lookup->finished.load();
        lookup->release();
        co_return out;
    };

    const auto out = sched.run_to_completion(test(&sched, &lookup));

    CAPTURE(out.failed);
    REQUIRE(out.failed.empty());
    REQUIRE_FALSE(out.result.has_value());
    CHECK(out.result.error() == std::errc::operation_canceled);
    CHECK(out.elapsed < 5s);
    CHECK(out.finished_then == 0);
    CHECK(lookup.finished.load() == 2);
}

TEST_CASE("a lookup whose callers have all left doesn't start", "[dns][resolver][stop]")
{
    test::blocking_lookup lookup;

    // the first lookup's caller, which waits for it to finish; and what went wrong in the test's own steps.
    std::vector<caller> first{caller{.host = "first.test"}};
    caller_counts       counts;
    std::string         failed;

    // one thread for blocking calls, which the first lookup holds, so the second waits for it.
    net::io::scheduler sched{make_pool(), {.blocking_threads = 1}};
    net::util::defer   release{[&lookup] noexcept { lookup.release(); }};

    const auto test = [](net::io::scheduler*    sched,
                         test::blocking_lookup* lookup,
                         std::span<caller>      first,
                         caller_counts*         counts,
                         std::string*           failed) -> task<resolve_result>
    {
        // lets its calls go however this ends (the scheduler waits for them).
        const net::util::defer let_go{[lookup] noexcept { lookup->release(); }};
        const auto             fail = [failed](std::string what)
        {
            *failed = std::move(what);
            return resolve_result{std::unexpected(std::error_code{})};
        };

        if (!start(sched, &lookup->resolver, first, counts)) co_return fail("spawn failed");
        if (!co_await wait_started(sched, lookup, 1)) co_return fail("the first lookup didn't start");

        // waits for the thread, until its caller leaves: then it's stopped, so it isn't started once it has one.
        auto result =
            co_await lookup->resolver.resolve(sched, "second.test", {.deadline = net::io::clock::now() + 50ms});

        // NOTE: the first lookup's second call (its other family's) waits for the thread too, and is made once it's
        //       released: its caller is waited for, so stopping the scheduler doesn't stop it first.
        lookup->release();
        if (!co_await wait_count(sched, &counts->returned, 1)) co_return fail("the first caller didn't return");
        co_return result;
    };

    const auto result = sched.run_to_completion(test(&sched, &lookup, first, &counts, &failed));

    CAPTURE(failed);
    REQUIRE(failed.empty());
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == std::errc::timed_out);
    CHECK(first[0].result == lookup.answer);

    // only the first's calls (a call for each family) were made.
    CHECK(lookup.started.load() == 2);
    CHECK(lookup.finished.load() == 2);
}

TEST_CASE("shutting the scheduler down waits for a lookup in flight", "[dns][resolver][shutdown]")
{
    test::blocking_lookup lookup;

    // 1 once its caller has given up (2 if it had an answer instead, 3 if the lookup's calls didn't start).
    std::atomic<int> left{0};

    net::io::scheduler sched{make_pool()};
    net::util::defer   release{[&lookup] noexcept { lookup.release(); }};

    REQUIRE(sched.spawn(
        [](net::io::scheduler* sched, test::blocking_lookup* lookup, std::atomic<int>* left) -> task<>
        {
            // a share of the lookup, held until the caller has given up, so that both families' calls have been
            // given threads before its deadline can pass: a lookup whose callers have all left before then never
            // makes them.
            auto held = lookup->resolver.start_resolving(sched, "slow.test", net::protocol::not_care);
            if (!held.has_value() || !co_await wait_started(sched, lookup, 2))
            {
                left->store(3);
                left->notify_all();
                co_return;
            }

            auto result =
                co_await lookup->resolver.resolve(sched, "slow.test", {.deadline = net::io::clock::now() + 100ms});
            left->store(result.has_value() ? 2 : 1);
            left->notify_all();
        }(&sched, &lookup, &left)));

    left.wait(0);
    REQUIRE(left.load() == 1);

    // releases the lookup a little after shutdown() starts waiting. NOTE: timed, as nothing says when shutdown() has
    // begun to wait: if the lookup's released before then, the test passes without shutdown() having waited for it.
    std::jthread releaser{[&lookup]
                          {
                              std::this_thread::sleep_for(100ms);
                              lookup.release();
                          }};

    sched.shutdown();
    CHECK(lookup.finished.load() == 2);
}

TEST_CASE("concurrent lookups of one name share one", "[dns][resolver][merge]")
{
    struct test_case
    {
        resolve_result answer{std::unexpected(std::error_code{})};
    };

    // an error is shared as addresses are. (Addresses sorted by RFC 6724, as the lookup sorts them, whatever the
    // machine's addresses are: see "a lookup of both families sorts their addresses together".)
    auto tc = GENERATE(
        test_case{
            .answer = std::vector<net::ip_addr>{net::ipv4_addr::loopback(), test_ipv6}
    },
        test_case{.answer = std::unexpected(net::make_gai_error_code(EAI_NONAME))});

    test::blocking_lookup lookup;
    lookup.answer = tc.answer;

    // the same name, spelled two ways.
    std::vector<caller> callers(8);
    for (std::size_t i = 0; i < callers.size(); ++i) callers[i].host = i % 2 == 0 ? "shared.test" : "Shared.TEST";
    caller_counts counts;

    net::io::scheduler sched{one_worker()};
    net::util::defer   release{[&lookup] noexcept { lookup.release(); }};

    const auto test = [](net::io::scheduler*    sched,
                         test::blocking_lookup* lookup,
                         std::span<caller>      callers,
                         caller_counts*         counts) -> task<std::string>
    {
        // lets its calls go however this ends (the scheduler waits for them).
        const net::util::defer let_go{[lookup] noexcept { lookup->release(); }};

        if (!start(sched, &lookup->resolver, callers, counts)) co_return "spawn failed";
        if (!co_await wait_count(sched, &counts->called, callers.size())) co_return "the callers didn't all call";
        if (!co_await wait_started(sched, lookup, 1)) co_return "the lookup didn't start";

        lookup->release();
        if (!co_await wait_count(sched, &counts->returned, callers.size())) co_return "the callers didn't all return";
        co_return "";
    };

    const auto failed = sched.run_to_completion(test(&sched, &lookup, callers, &counts));
    CAPTURE(failed);
    REQUIRE(failed.empty());

    CHECK(lookup.started.load() == 2); // a call for each family
    for (const auto& c : callers)
    {
        CAPTURE(c.host);
        CHECK(c.result == tc.answer);
    }

    if (!tc.answer.has_value())
    {
        for (const auto& c : callers)
        {
            REQUIRE_FALSE(c.result.has_value());
            CHECK(c.result.error() == net::dns::condition::not_found);
        }
    }
}

TEST_CASE("lookups of different names or families don't share", "[dns][resolver][merge]")
{
    test::blocking_lookup lookup;

    // four lookups (the last caller shares the first's), of six calls (one for each family), as many as the threads
    // for blocking calls.
    std::vector<caller> callers{
        caller{.host = "a.test"},
        caller{.host = "a.test", .proto = net::protocol::ipv4},
        caller{.host = "a.test", .proto = net::protocol::ipv6},
        caller{.host = "b.test"},
        caller{.host = "A.test"},
    };
    caller_counts counts;

    net::io::scheduler sched{one_worker(), {.blocking_threads = 6}};
    net::util::defer   release{[&lookup] noexcept { lookup.release(); }};

    const auto test = [](net::io::scheduler*    sched,
                         test::blocking_lookup* lookup,
                         std::span<caller>      callers,
                         caller_counts*         counts) -> task<std::string>
    {
        // lets its calls go however this ends (the scheduler waits for them).
        const net::util::defer let_go{[lookup] noexcept { lookup->release(); }};

        if (!start(sched, &lookup->resolver, callers, counts)) co_return "spawn failed";
        if (!co_await wait_count(sched, &counts->called, callers.size())) co_return "the callers didn't all call";
        if (!co_await wait_started(sched, lookup, 6)) co_return "the lookups didn't all start";

        lookup->release();
        if (!co_await wait_count(sched, &counts->returned, callers.size())) co_return "the callers didn't all return";
        co_return "";
    };

    const auto failed = sched.run_to_completion(test(&sched, &lookup, callers, &counts));
    CAPTURE(failed);
    REQUIRE(failed.empty());

    CHECK(lookup.started.load() == 6);
    for (const auto& c : callers)
    {
        CAPTURE(c.host, c.proto);
        CHECK(c.result == lookup.answer_for(c.proto));
    }
}

TEST_CASE("a caller that leaves a shared lookup leaves the others waiting", "[dns][resolver][merge]")
{
    test::blocking_lookup lookup;

    std::stop_source    stop;
    std::vector<caller> callers{
        caller{.host = "shared.test"},
        caller{.host = "shared.test", .deadline = net::io::clock::now() + 100ms},
        caller{.host = "shared.test", .stop = stop.get_token()},
        caller{.host = "shared.test"},
    };
    caller_counts counts;

    net::io::scheduler sched{one_worker()};
    net::util::defer   release{[&lookup] noexcept { lookup.release(); }};

    const auto test = [](net::io::scheduler*    sched,
                         test::blocking_lookup* lookup,
                         std::span<caller>      callers,
                         caller_counts*         counts,
                         std::stop_source*      stop) -> task<std::pair<int, std::string>>
    {
        // lets its calls go however this ends (the scheduler waits for them).
        const net::util::defer let_go{[lookup] noexcept { lookup->release(); }};

        const auto fail = [](std::string what) { return std::pair{-1, std::move(what)}; };

        if (!start(sched, &lookup->resolver, callers, counts)) co_return fail("spawn failed");
        if (!co_await wait_count(sched, &counts->called, callers.size())) co_return fail("the callers didn't all call");
        if (!co_await wait_started(sched, lookup, 1)) co_return fail("the lookup didn't start");

        // one leaves at its deadline, and one when it's stopped: the others still wait.
        stop->request_stop();
        if (!co_await wait_count(sched, &counts->returned, 2)) co_return fail("the two didn't leave");

        const auto finished_then = lookup->finished.load();
        lookup->release();
        if (!co_await wait_count(sched, &counts->returned, callers.size()))
            co_return fail("the callers didn't all return");
        co_return std::pair{finished_then, std::string{}};
    };

    const auto [finished_then, failed] = sched.run_to_completion(test(&sched, &lookup, callers, &counts, &stop));
    CAPTURE(failed);
    REQUIRE(failed.empty());
    CHECK(finished_then == 0);
    CHECK(lookup.started.load() == 2); // a call for each family

    REQUIRE_FALSE(callers[1].result.has_value());
    CHECK(callers[1].result.error() == std::errc::timed_out);
    REQUIRE_FALSE(callers[2].result.has_value());
    CHECK(callers[2].result.error() == std::errc::operation_canceled);

    CHECK(callers[0].result == lookup.answer);
    CHECK(callers[3].result == lookup.answer);
}

TEST_CASE("a shared lookup whose callers have all left is stopped, and not shared", "[dns][resolver][merge]")
{
    test::blocking_lookup lookup;

    // the first holds the only thread for blocking calls, so the lookup the next three share waits for it. (The one
    // that starts it stays until it's stopped, so the others share it, whenever their deadlines pass.)
    std::stop_source    stop;
    std::vector<caller> callers{
        caller{.host = "held.test"},
        caller{.host = "queued.test", .stop = stop.get_token()},
        caller{.host = "queued.test", .deadline = net::io::clock::now() + 50ms},
        caller{.host = "queued.test", .deadline = net::io::clock::now() + 100ms},
    };
    // then one for the same name, once they've left.
    caller        later{.host = "queued.test"};
    caller_counts counts;

    net::io::scheduler sched{one_worker(), {.blocking_threads = 1}};
    net::util::defer   release{[&lookup] noexcept { lookup.release(); }};

    const auto test = [](net::io::scheduler*    sched,
                         test::blocking_lookup* lookup,
                         std::span<caller>      callers,
                         caller*                later,
                         caller_counts*         counts,
                         std::stop_source*      stop) -> task<std::string>
    {
        // lets its calls go however this ends (the scheduler waits for them).
        const net::util::defer let_go{[lookup] noexcept { lookup->release(); }};

        if (!start(sched, &lookup->resolver, callers, counts)) co_return "spawn failed";
        if (!co_await wait_count(sched, &counts->called, callers.size())) co_return "the callers didn't all call";
        if (!co_await wait_started(sched, lookup, 1)) co_return "the first lookup didn't start";

        stop->request_stop();
        if (!co_await wait_count(sched, &counts->returned, 3)) co_return "the three didn't leave";

        // a lookup of its own, rather than the one they left, which was stopped (and would fail with
        // operation_canceled for their reasons).
        if (!start(sched, &lookup->resolver, {later, 1}, counts)) co_return "spawn failed";
        if (!co_await wait_count(sched, &counts->called, callers.size() + 1)) co_return "the later caller didn't call";

        lookup->release();
        if (!co_await wait_count(sched, &counts->returned, callers.size() + 1))
            co_return "the callers didn't all return";
        co_return "";
    };

    const auto failed = sched.run_to_completion(test(&sched, &lookup, callers, &later, &counts, &stop));
    CAPTURE(failed);
    REQUIRE(failed.empty());

    // run_to_completion() waited for every lookup: the one they left never started. (The others made a call for each
    // family.)
    CHECK(lookup.started.load() == 4);
    CHECK(lookup.finished.load() == 4);

    CHECK(callers[0].result == lookup.answer);
    for (std::size_t i = 1; i < callers.size(); ++i)
    {
        CAPTURE(i);
        CHECK_FALSE(callers[i].result.has_value());
    }
    CHECK(later.result == lookup.answer);
}

TEST_CASE("a lookup that has finished isn't shared", "[dns][resolver][merge]")
{
    test::blocking_lookup lookup;
    lookup.release(); // answers at once

    net::io::scheduler sched{make_pool()};

    struct outcome
    {
        resolve_result first{std::unexpected(std::error_code{})};
        int            started_then = 0;
        resolve_result second{std::unexpected(std::error_code{})};
    };

    const auto test = [](net::io::scheduler* sched, test::blocking_lookup* lookup) -> task<outcome>
    {
        outcome out;
        out.first        = co_await lookup->resolver.resolve(sched, "again.test");
        out.started_then = lookup->started.load();
        out.second       = co_await lookup->resolver.resolve(sched, "again.test");
        co_return out;
    };

    const auto out = sched.run_to_completion(test(&sched, &lookup));
    CHECK(out.first.has_value());
    CHECK(out.second.has_value());
    CHECK(out.started_then == 2); // a call for each family
    CHECK(lookup.started.load() == 4);
}

namespace
{

// family_answers is a system back end whose calls answer at once, each family with its own result.
struct family_answers
{
    resolve_result ipv6{std::unexpected(std::error_code{})};
    resolve_result ipv4{std::unexpected(std::error_code{})};

    std::atomic<int> ipv6_calls{0};
    std::atomic<int> ipv4_calls{0};
    std::atomic<int> other_calls{0};

    net::dns::resolver resolver{
        {
         .use           = net::dns::resolver::backend::system,
         .system_lookup = [this](const std::string& /*host*/, net::protocol proto) -> resolve_result
            {
                switch (proto)
                {
                case net::protocol::ipv6: ipv6_calls.fetch_add(1); return ipv6;
                case net::protocol::ipv4: ipv4_calls.fetch_add(1); return ipv4;
                case net::protocol::not_care: break;
                }
                other_calls.fetch_add(1);
                return std::unexpected(std::make_error_code(std::errc::invalid_argument));
            }, }
    };
};

// wait_news waits (in a task) until res has something new after version, returning what it has then: what it had, if
// the scheduler's stopped, or nothing new came within 10 seconds.
task<net::dns::detail::found>
wait_news(net::io::scheduler* sched, const net::dns::detail::resolution* res, std::uint64_t version)
{
    const auto until = net::io::clock::now() + 10s;
    while (res->news().load() == version)
    {
        if (auto err = co_await sched->wait_on(res->news(), version, until); err) break;
    }
    co_return res->look();
}

}

TEST_CASE("a lookup of both families sorts their addresses together", "[dns][resolver][families]")
{
    struct test_case
    {
        std::vector<net::ip_addr> ipv6{};
        std::vector<net::ip_addr> ipv4{};

        // the order, if it's the same on any machine.
        std::vector<net::ip_addr> exactly{};
    };

    // each family's calls answer in their own order; the result is RFC 6724's (which, for the first, is the same on
    // any machine: see test_ipv6).
    auto tc = GENERATE(
        test_case{
            .ipv6    = {test_ipv6},
            .ipv4    = {net::ipv4_addr::loopback()},
            .exactly = {net::ipv4_addr::loopback(), test_ipv6}
    },
        test_case{.ipv6 = {net::ipv6_addr::loopback()}, .ipv4 = {net::ipv4_addr::loopback()}},
        test_case{
            .ipv6 = {test_ipv6, net::ipv6_addr::loopback()},
            .ipv4 = {net::ipv4_addr{192, 0, 2, 1}, net::ipv4_addr::loopback()},
        });

    family_answers answers;
    answers.ipv6 = tc.ipv6;
    answers.ipv4 = tc.ipv4;

    net::io::scheduler sched{make_pool()};

    auto result = sched.run_to_completion(answers.resolver.resolve(&sched, "both.test"));
    REQUIRE(result.has_value());

    auto expected = tc.ipv6;
    expected.insert(expected.end(), tc.ipv4.begin(), tc.ipv4.end());
    net::dns::detail::sort_by_rfc6724(expected);
    CHECK(*result == expected);
    if (!tc.exactly.empty()) CHECK(*result == tc.exactly);

    // a call for each family, and nothing else.
    CHECK(answers.ipv6_calls.load() == 1);
    CHECK(answers.ipv4_calls.load() == 1);
    CHECK(answers.other_calls.load() == 0);
}

TEST_CASE("a lookup of one family makes one call", "[dns][resolver][families]")
{
    family_answers answers;
    answers.ipv6 = std::vector<net::ip_addr>{test_ipv6};
    answers.ipv4 = std::vector<net::ip_addr>{net::ipv4_addr::loopback()};

    const auto proto = GENERATE(net::protocol::ipv4, net::protocol::ipv6);
    CAPTURE(proto);

    net::io::scheduler sched{make_pool()};

    auto result = sched.run_to_completion(answers.resolver.resolve(&sched, "one.test", {.proto = proto}));
    CHECK(result == (proto == net::protocol::ipv4 ? answers.ipv4 : answers.ipv6));
    CHECK(answers.ipv4_calls.load() == (proto == net::protocol::ipv4 ? 1 : 0));
    CHECK(answers.ipv6_calls.load() == (proto == net::protocol::ipv6 ? 1 : 0));
    CHECK(answers.other_calls.load() == 0);
}

TEST_CASE("a lookup of both families fails only if both do", "[dns][resolver][families][errors]")
{
    const auto error = [](int code) { return resolve_result{std::unexpected(net::make_gai_error_code(code))}; };

    struct test_case
    {
        std::string    name{};
        resolve_result ipv6{std::unexpected(std::error_code{})};
        resolve_result ipv4{std::unexpected(std::error_code{})};
        resolve_result expected{std::unexpected(std::error_code{})};
    };

    const std::vector<net::ip_addr> v6{test_ipv6};
    const std::vector<net::ip_addr> v4{
        net::ipv4_addr{192, 0, 2, 1}
    };

    auto tc = GENERATE_COPY(
        // one failing (e.g. no AAAA records, or no IPv6 on the host) isn't an error.
        test_case{.name = "no IPv6", .ipv6 = error(EAI_NONAME), .ipv4 = v4, .expected = v4},
        test_case{.name = "IPv4 failed", .ipv6 = v6, .ipv4 = error(EAI_AGAIN), .expected = v6},
        // both failing: a temporary failure, if only one is one...
        test_case{.name     = "IPv6 timed out",
                  .ipv6     = error(EAI_AGAIN),
                  .ipv4     = error(EAI_NONAME),
                  .expected = error(EAI_AGAIN)},
        test_case{.name     = "IPv4 timed out",
                  .ipv6     = error(EAI_NONAME),
                  .ipv4     = error(EAI_AGAIN),
                  .expected = error(EAI_AGAIN)},
        // ... and otherwise IPv4's.
        test_case{.name     = "neither found",
                  .ipv6     = error(EAI_FAIL),
                  .ipv4     = error(EAI_AGAIN),
                  .expected = error(EAI_AGAIN)},
        test_case{.name     = "other errors",
                  .ipv6     = error(EAI_NONAME),
                  .ipv4     = error(EAI_MEMORY),
                  .expected = error(EAI_MEMORY)},
        test_case{.name     = "other errors, the other way",
                  .ipv6     = error(EAI_MEMORY),
                  .ipv4     = error(EAI_NONAME),
                  .expected = error(EAI_NONAME)});
    CAPTURE(tc.name);

    family_answers answers;
    answers.ipv6 = tc.ipv6;
    answers.ipv4 = tc.ipv4;

    net::io::scheduler sched{make_pool()};

    auto result = sched.run_to_completion(answers.resolver.resolve(&sched, "failing.test"));
    CHECK(result == tc.expected);
}

TEST_CASE("a resolution is told of each family's addresses as they come in", "[dns][resolver][resolution]")
{
    test::blocking_lookup lookup;
    lookup.answer = std::vector<net::ip_addr>{net::ipv4_addr::loopback(), test_ipv6};

    const auto first = GENERATE(net::protocol::ipv4, net::protocol::ipv6);
    const auto other = first == net::protocol::ipv4 ? net::protocol::ipv6 : net::protocol::ipv4;
    CAPTURE(first);

    // what the resolution had found: at the start, once the first family's in, and once the other's is.
    struct seen
    {
        net::dns::detail::found    start{};
        net::dns::detail::found    first{};
        net::dns::detail::found    done{};
        net::io::clock::time_point released{};
        std::string                failed{}; // what went wrong in the test's own steps
    };

    net::io::scheduler sched{make_pool()};
    net::util::defer   release{[&lookup] noexcept { lookup.release(); }};

    const auto test = [](net::io::scheduler*    sched,
                         test::blocking_lookup* lookup,
                         net::protocol          first,
                         net::protocol          other) -> task<seen>
    {
        // lets its calls go however this ends (the scheduler waits for them).
        const net::util::defer let_go{[lookup] noexcept { lookup->release(); }};

        auto res = lookup->resolver.start_resolving(sched, "both.test", net::protocol::not_care);
        if (!res.has_value()) co_return seen{.failed = "start_resolving: " + res.error().message()};

        seen out{.start = res->look()};

        out.released = net::io::clock::now();
        lookup->release(first);
        out.first = co_await wait_news(sched, &*res, out.start.version);

        lookup->release(other);
        out.done = co_await wait_news(sched, &*res, out.first.version);
        co_return out;
    };

    const auto out = sched.run_to_completion(test(&sched, &lookup, first, other));
    CAPTURE(out.failed);
    REQUIRE(out.failed.empty());

    CHECK_FALSE(out.start.done);
    CHECK(out.start.addrs.empty());

    CHECK_FALSE(out.first.done);
    CHECK(out.first.family == first);
    CHECK(out.first.addrs == lookup.answer_for(first));
    CHECK(out.first.at >= out.released);
    CHECK(out.first.version > out.start.version);

    // all of them, sorted.
    CHECK(out.done.done);
    CHECK(out.done.addrs == *lookup.answer);
    CHECK(out.done.version > out.first.version);
}

TEST_CASE("a resolution isn't told of a family that fails", "[dns][resolver][resolution]")
{
    test::blocking_lookup lookup;
    lookup.answer = std::vector<net::ip_addr>{net::ipv4_addr::loopback()}; // no IPv6 addresses

    // what the resolution had found once IPv6's call had failed, and once it's done.
    struct seen
    {
        net::dns::detail::found after_ipv6{};
        net::dns::detail::found done{};
        std::string             failed{}; // what went wrong in the test's own steps
    };

    // IPv6's failure must be taken before IPv4's addresses (were IPv4's first, they'd be told of, and the result after
    // them): with one thread for blocking calls, IPv4's call is made once IPv6's has ended, which has by then handed
    // its result to the one worker (queued, as from another thread); and that worker runs what's queued in order, so it
    // takes IPv6's result before IPv4's, and before the test, which yields once IPv4's call has started.
    const auto         pool = one_worker();
    net::io::scheduler sched{pool, {.blocking_threads = 1}};
    net::util::defer   release{[&lookup] noexcept { lookup.release(); }};

    const auto test =
        [](net::io::scheduler* sched, net::coro::thread_pool* pool, test::blocking_lookup* lookup) -> task<seen>
    {
        // lets its calls go however this ends (the scheduler waits for them).
        const net::util::defer let_go{[lookup] noexcept { lookup->release(); }};

        auto res = lookup->resolver.start_resolving(sched, "v4.test", net::protocol::not_care);
        if (!res.has_value()) co_return seen{.failed = "start_resolving: " + res.error().message()};

        // IPv6's call (made first: IPv4's is in a task of its own) fails: nothing to tell.
        lookup->release(net::protocol::ipv6);
        if (!co_await wait_started(sched, lookup, 2)) co_return seen{.failed = "IPv4's call didn't start"};
        co_await pool->yield();

        seen out{.after_ipv6 = res->look()};
        lookup->release(net::protocol::ipv4);
        out.done = co_await wait_news(sched, &*res, 0);
        co_return out;
    };

    const auto out = sched.run_to_completion(test(&sched, pool.get(), &lookup));
    CAPTURE(out.failed);
    REQUIRE(out.failed.empty());

    const auto& after_ipv6 = out.after_ipv6;
    const auto& done       = out.done;

    CHECK_FALSE(after_ipv6.done);
    CHECK(after_ipv6.addrs.empty());
    CHECK(after_ipv6.version == 0);

    CHECK(done.done);
    CHECK(done.addrs == *lookup.answer);
    CHECK(done.version == 1);
}

TEST_CASE("a shared lookup tells what's in so far to the callers that join it", "[dns][resolver][resolution][merge]")
{
    test::blocking_lookup lookup;
    lookup.answer = std::vector<net::ip_addr>{net::ipv4_addr::loopback(), test_ipv6};

    std::vector<caller> callers{caller{.host = "shared.test"}};
    caller_counts       counts;

    // what a resolution joining it found, once IPv4's addresses were in; what one joining after found at once; and
    // whether resolve()'s caller had returned by then.
    struct seen
    {
        net::dns::detail::found joined{};
        net::dns::detail::found later{};
        std::uint64_t           returned_then = 0;
        net::dns::detail::found done{};
        std::string             failed{}; // what went wrong in the test's own steps
    };

    net::io::scheduler sched{one_worker()};
    net::util::defer   release{[&lookup] noexcept { lookup.release(); }};

    const auto test = [](net::io::scheduler*    sched,
                         test::blocking_lookup* lookup,
                         std::span<caller>      callers,
                         caller_counts*         counts) -> task<seen>
    {
        // lets its calls go however this ends (the scheduler waits for them).
        const net::util::defer let_go{[lookup] noexcept { lookup->release(); }};

        // resolve()'s caller starts the lookup, then a resolution joins it.
        if (!start(sched, &lookup->resolver, callers, counts)) co_return seen{.failed = "spawn failed"};
        if (!co_await wait_started(sched, lookup, 2)) co_return seen{.failed = "the lookup's calls didn't start"};

        auto res = lookup->resolver.start_resolving(sched, "SHARED.test", net::protocol::not_care);
        if (!res.has_value()) co_return seen{.failed = "start_resolving: " + res.error().message()};

        seen out;
        lookup->release(net::protocol::ipv4);
        out.joined        = co_await wait_news(sched, &*res, 0);
        out.returned_then = counts->returned.load();

        auto later = lookup->resolver.start_resolving(sched, "shared.test", net::protocol::not_care);
        if (!later.has_value()) co_return seen{.failed = "start_resolving later: " + later.error().message()};
        out.later = later->look();

        lookup->release(net::protocol::ipv6);
        out.done = co_await wait_news(sched, &*res, out.joined.version);

        if (!co_await wait_count(sched, &counts->returned, callers.size()))
            co_return seen{.failed = "the caller didn't return"};
        co_return out;
    };

    const auto out = sched.run_to_completion(test(&sched, &lookup, callers, &counts));
    CAPTURE(out.failed);
    REQUIRE(out.failed.empty());

    // one lookup: a call for each family.
    CHECK(lookup.started.load() == 2);

    CHECK_FALSE(out.joined.done);
    CHECK(out.joined.family == net::protocol::ipv4);
    CHECK(out.joined.addrs == std::vector<net::ip_addr>{net::ipv4_addr::loopback()});
    CHECK(out.returned_then == 0);

    // the same, when it came in.
    CHECK_FALSE(out.later.done);
    CHECK(out.later.addrs == out.joined.addrs);
    CHECK(out.later.at == out.joined.at);
    CHECK(out.later.version == out.joined.version);

    CHECK(out.done.done);
    CHECK(out.done.addrs == *lookup.answer);
    CHECK(callers[0].result == lookup.answer);
}

TEST_CASE("a resolution of an IP address is done at once", "[dns][resolver][resolution][literal]")
{
    test::blocking_lookup lookup;

    net::io::scheduler sched{make_pool()};
    net::util::defer   release{[&lookup] noexcept { lookup.release(); }};

    auto res = lookup.resolver.start_resolving(&sched, "::1", net::protocol::not_care);
    REQUIRE(res.has_value());

    const auto found = res->look();
    CHECK(found.done);
    CHECK(found.addrs == std::vector<net::ip_addr>{net::ipv6_addr::loopback()});

    CHECK(lookup.resolver.start_resolving(&sched, "::1", net::protocol::ipv4).error()
          == std::errc::address_family_not_supported);
    CHECK(lookup.resolver.start_resolving(&sched, "a..b", net::protocol::not_care).error()
          == std::errc::invalid_argument);
    CHECK(lookup.started.load() == 0);
}

TEST_CASE("the system's resolver, and the default one, find localhost", "[dns][resolver]")
{
    // the default resolver's back end is automatic's choice, which may be either.
    const bool               use_default = GENERATE(false, true);
    const net::dns::resolver system{{.use = net::dns::resolver::backend::system}};
    const auto&              resolver = use_default ? net::dns::default_resolver() : system;
    CAPTURE(use_default);

    net::io::scheduler sched{make_pool()};

    auto results = sched.run_to_completion(resolver.resolve(&sched, "localhost"));
    REQUIRE(results.has_value());
    CHECK_FALSE(results->empty());

    for (const auto& addr : *results)
    {
        // a reversed address prints, and classifies, differently.
        CAPTURE(addr.to_string());
        if (addr.is_ipv4()) CHECK(addr.to_ipv4().is_loopback());
        if (addr.is_ipv6()) CHECK(addr.to_ipv6() == net::ipv6_addr::loopback());
    }
}

TEST_CASE("the system's resolver finds a public name", "[dns][resolver][.network]")
{
    const net::dns::resolver resolver{{.use = net::dns::resolver::backend::system}};
    net::io::scheduler       sched{make_pool()};

    auto results = sched.run_to_completion(resolver.resolve(&sched, "google.com"));
    REQUIRE(results.has_value());
    CHECK_FALSE(results->empty());
}

TEST_CASE("the system's resolver reports names that don't exist", "[dns][resolver][.network]")
{
    const net::dns::resolver resolver{{.use = net::dns::resolver::backend::system}};
    net::io::scheduler       sched{make_pool()};

    // .invalid never resolves (RFC 6761).
    auto results = sched.run_to_completion(resolver.resolve(&sched, "nothing.invalid"));
    REQUIRE_FALSE(results.has_value());
    CAPTURE(results.error().message());
    CHECK(results.error() == net::dns::condition::not_found);
}
