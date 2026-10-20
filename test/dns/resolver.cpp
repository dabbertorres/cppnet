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
        CHECK(looked_up.load() == 1);
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
        const auto start  = net::io::clock::now();
        auto       result = co_await lookup->resolver.resolve(sched, "slow.test", {.deadline = start + 100ms});
        timed      out{.result = std::move(result), .elapsed = net::io::clock::now() - start};

        out.finished_then = lookup->finished.load();
        lookup->release(); // its result, once it has one, is dropped.
        co_return out;
    };

    const auto out = sched.run_to_completion(test(&sched, &lookup));

    REQUIRE_FALSE(out.result.has_value());
    CHECK(out.result.error() == std::errc::timed_out);
    CHECK(out.result.error() == net::io::status_condition::timed_out);
    CHECK(out.elapsed >= 100ms);
    CHECK(out.elapsed < 5s);
    CHECK(lookup.started.load() == 1);
    CHECK(out.finished_then == 0);

    // run_to_completion() waited for the lookup to finish.
    CHECK(lookup.finished.load() == 1);
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

            // stops once the lookup has started.
            REQUIRE(sched->spawn(
                [](net::io::scheduler* sched, test::blocking_lookup* lookup, std::stop_source source) -> task<>
                {
                    if (co_await wait_started(sched, lookup, 1)) source.request_stop();
                }(sched, lookup, source)));

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

        REQUIRE_FALSE(out.result.has_value());
        CHECK(out.result.error() == std::errc::operation_canceled);
        CHECK(out.elapsed < 5s);
        CHECK(out.finished_then == 0);
        CHECK(lookup.started.load() == 1);
        CHECK(lookup.finished.load() == 1);
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
        REQUIRE(sched->spawn(
            [](net::io::scheduler* sched, test::blocking_lookup* lookup) -> task<>
            {
                if (co_await wait_started(sched, lookup, 1)) sched->request_stop();
            }(sched, lookup)));

        const auto start  = net::io::clock::now();
        auto       result = co_await lookup->resolver.resolve(sched, "slow.test", {.deadline = start + 10s});
        timed      out{.result = std::move(result), .elapsed = net::io::clock::now() - start};

        out.finished_then = lookup->finished.load();
        lookup->release();
        co_return out;
    };

    const auto out = sched.run_to_completion(test(&sched, &lookup));

    REQUIRE_FALSE(out.result.has_value());
    CHECK(out.result.error() == std::errc::operation_canceled);
    CHECK(out.elapsed < 5s);
    CHECK(out.finished_then == 0);
    CHECK(lookup.finished.load() == 1);
}

TEST_CASE("a lookup whose callers have all left doesn't start", "[dns][resolver][stop]")
{
    test::blocking_lookup lookup;

    // one thread for blocking calls, which the first lookup holds, so the second waits for it.
    net::io::scheduler sched{make_pool(), {.blocking_threads = 1}};
    net::util::defer   release{[&lookup] noexcept { lookup.release(); }};

    const auto test = [](net::io::scheduler* sched, test::blocking_lookup* lookup) -> task<resolve_result>
    {
        REQUIRE(sched->spawn([](net::io::scheduler* sched, test::blocking_lookup* lookup) -> task<>
                             { (void)co_await lookup->resolver.resolve(sched, "first.test"); }(sched, lookup)));
        const auto started = co_await wait_started(sched, lookup, 1);
        REQUIRE(started);

        // waits for the thread, until its caller leaves: then it's stopped, so it isn't started once it has one.
        auto result =
            co_await lookup->resolver.resolve(sched, "second.test", {.deadline = net::io::clock::now() + 50ms});

        lookup->release();
        co_return result;
    };

    const auto result = sched.run_to_completion(test(&sched, &lookup));

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == std::errc::timed_out);

    // run_to_completion() waited for both lookups: only the first was called.
    CHECK(lookup.started.load() == 1);
    CHECK(lookup.finished.load() == 1);
}

TEST_CASE("shutting the scheduler down waits for a lookup in flight", "[dns][resolver][shutdown]")
{
    test::blocking_lookup lookup;

    net::io::scheduler sched{make_pool()};
    net::util::defer   release{[&lookup] noexcept { lookup.release(); }};

    // 1 once its caller has given up (2 if it had an answer instead).
    std::atomic<int> left{0};
    REQUIRE(sched.spawn(
        [](net::io::scheduler* sched, test::blocking_lookup* lookup, std::atomic<int>* left) -> task<>
        {
            auto result =
                co_await lookup->resolver.resolve(sched, "slow.test", {.deadline = net::io::clock::now() + 10ms});
            left->store(result.has_value() ? 2 : 1);
            left->notify_all();
        }(&sched, &lookup, &left)));

    REQUIRE(lookup.wait_started(1));
    left.wait(0);
    CHECK(left.load() == 1);

    // releases the lookup a little after shutdown() starts waiting.
    std::jthread releaser{[&lookup]
                          {
                              std::this_thread::sleep_for(100ms);
                              lookup.release();
                          }};

    sched.shutdown();
    CHECK(lookup.finished.load() == 1);
}

TEST_CASE("concurrent lookups of one name share one", "[dns][resolver][merge]")
{
    struct test_case
    {
        resolve_result answer{std::unexpected(std::error_code{})};
    };

    // an error is shared as addresses are.
    auto tc = GENERATE(
        test_case{
            .answer = std::vector<net::ip_addr>{net::ipv6_addr::loopback(), net::ipv4_addr::loopback()}
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
                         caller_counts*         counts) -> task<>
    {
        REQUIRE(start(sched, &lookup->resolver, callers, counts));

        const auto called = co_await wait_count(sched, &counts->called, callers.size());
        REQUIRE(called);
        const auto started = co_await wait_started(sched, lookup, 1);
        REQUIRE(started);

        lookup->release();
        const auto returned = co_await wait_count(sched, &counts->returned, callers.size());
        REQUIRE(returned);
    };

    sched.run_to_completion(test(&sched, &lookup, callers, &counts));

    CHECK(lookup.started.load() == 1);
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

    // four lookups (the last caller shares the first's), as many as the threads for blocking calls.
    std::vector<caller> callers{
        caller{.host = "a.test"},
        caller{.host = "a.test", .proto = net::protocol::ipv4},
        caller{.host = "a.test", .proto = net::protocol::ipv6},
        caller{.host = "b.test"},
        caller{.host = "A.test"},
    };
    caller_counts counts;

    net::io::scheduler sched{one_worker(), {.blocking_threads = 4}};
    net::util::defer   release{[&lookup] noexcept { lookup.release(); }};

    const auto test = [](net::io::scheduler*    sched,
                         test::blocking_lookup* lookup,
                         std::span<caller>      callers,
                         caller_counts*         counts) -> task<>
    {
        REQUIRE(start(sched, &lookup->resolver, callers, counts));

        const auto called = co_await wait_count(sched, &counts->called, callers.size());
        REQUIRE(called);
        const auto started = co_await wait_started(sched, lookup, 4);
        REQUIRE(started);

        lookup->release();
        const auto returned = co_await wait_count(sched, &counts->returned, callers.size());
        REQUIRE(returned);
    };

    sched.run_to_completion(test(&sched, &lookup, callers, &counts));

    CHECK(lookup.started.load() == 4);
    for (const auto& c : callers)
    {
        CAPTURE(c.host, c.proto);
        CHECK(c.result == lookup.answer);
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
                         std::stop_source*      stop) -> task<int>
    {
        REQUIRE(start(sched, &lookup->resolver, callers, counts));

        const auto called = co_await wait_count(sched, &counts->called, callers.size());
        REQUIRE(called);
        const auto started = co_await wait_started(sched, lookup, 1);
        REQUIRE(started);

        // one leaves at its deadline, and one when it's stopped: the others still wait.
        stop->request_stop();
        const auto left = co_await wait_count(sched, &counts->returned, 2);
        REQUIRE(left);

        const auto finished_then = lookup->finished.load();
        lookup->release();
        const auto returned = co_await wait_count(sched, &counts->returned, callers.size());
        REQUIRE(returned);
        co_return finished_then;
    };

    const auto finished_then = sched.run_to_completion(test(&sched, &lookup, callers, &counts, &stop));
    CHECK(finished_then == 0);
    CHECK(lookup.started.load() == 1);

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
                         std::stop_source*      stop) -> task<>
    {
        REQUIRE(start(sched, &lookup->resolver, callers, counts));

        const auto called = co_await wait_count(sched, &counts->called, callers.size());
        REQUIRE(called);
        const auto started = co_await wait_started(sched, lookup, 1);
        REQUIRE(started);

        stop->request_stop();
        const auto left = co_await wait_count(sched, &counts->returned, 3);
        REQUIRE(left);

        // a lookup of its own, rather than the one they left, which was stopped (and would fail with
        // operation_canceled for their reasons).
        REQUIRE(start(sched, &lookup->resolver, {later, 1}, counts));
        const auto later_called = co_await wait_count(sched, &counts->called, callers.size() + 1);
        REQUIRE(later_called);

        lookup->release();
        const auto returned = co_await wait_count(sched, &counts->returned, callers.size() + 1);
        REQUIRE(returned);
    };

    sched.run_to_completion(test(&sched, &lookup, callers, &later, &counts, &stop));

    // run_to_completion() waited for every lookup: the one they left never started.
    CHECK(lookup.started.load() == 2);
    CHECK(lookup.finished.load() == 2);

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

    const auto test = [](net::io::scheduler* sched, test::blocking_lookup* lookup) -> task<int>
    {
        auto first = co_await lookup->resolver.resolve(sched, "again.test");
        REQUIRE(first.has_value());
        const auto started_then = lookup->started.load();

        auto second = co_await lookup->resolver.resolve(sched, "again.test");
        REQUIRE(second.has_value());
        co_return started_then;
    };

    const auto started_then = sched.run_to_completion(test(&sched, &lookup));
    CHECK(started_then == 1);
    CHECK(lookup.started.load() == 2);
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

TEST_CASE("the system's resolver finds a public name", "[dns][resolver][network]")
{
    const net::dns::resolver resolver{{.use = net::dns::resolver::backend::system}};
    net::io::scheduler       sched{make_pool()};

    auto results = sched.run_to_completion(resolver.resolve(&sched, "google.com"));
    REQUIRE(results.has_value());
    CHECK_FALSE(results->empty());
}

TEST_CASE("the system's resolver reports names that don't exist", "[dns][resolver][network]")
{
    const net::dns::resolver resolver{{.use = net::dns::resolver::backend::system}};
    net::io::scheduler       sched{make_pool()};

    // .invalid never resolves (RFC 6761).
    auto results = sched.run_to_completion(resolver.resolve(&sched, "nothing.invalid"));
    REQUIRE_FALSE(results.has_value());
    CAPTURE(results.error().message());
    CHECK(results.error() == net::dns::condition::not_found);
}
