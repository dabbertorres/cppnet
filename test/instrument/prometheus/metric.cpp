#include "instrument/prometheus/metric.hpp"

#include <cstddef>
#include <exception> // IWYU pragma: keep
#include <string>
#include <thread>
#include <vector>

#include <catch.hpp>

#include <catch2/catch_test_macros.hpp>

#include "coro/sync_wait.hpp"
#include "instrument/prometheus/counter.hpp"
#include "instrument/prometheus/gauge.hpp"
#include "instrument/prometheus/registry.hpp"
#include "io/string_writer.hpp"

using net::instrument::prometheus::counter;
using net::instrument::prometheus::gauge;
using net::instrument::prometheus::registry;
using net::io::string_writer;

namespace
{

template<typename Metric>
std::string encode(const Metric& m)
{
    string_writer<char> writer;
    auto                res = test::sync_wait(m.encode(writer));
    REQUIRE_FALSE(res.err);
    return writer.build();
}

}

TEST_CASE("counter encodes", "[instrument][prometheus][counter][encode]")
{
    auto c = counter{"requests_total", "Requests served.", {{"code", "200"}}};
    c += 2.5;
    ++c;

    // NOTE: no timestamp.
    CHECK(encode(c) == R"(# HELP requests_total Requests served.
# TYPE requests_total counter
requests_total{code="200"} 3.5
)");
}

TEST_CASE("gauge encodes", "[instrument][prometheus][gauge][encode]")
{
    auto g = gauge{"temperature"};
    g.set(-1.25);

    CHECK(encode(g) == "# TYPE temperature gauge\ntemperature -1.25\n");
}

TEST_CASE("counter increments from many threads aren't lost", "[instrument][prometheus][counter]")
{
    constexpr std::size_t threads    = 8;
    constexpr std::size_t increments = 50'000;

    auto c = counter{"test"};

    {
        std::vector<std::jthread> workers;
        for (std::size_t t = 0; t < threads; ++t)
        {
            workers.emplace_back(
                [&c]
                {
                    for (std::size_t i = 0; i < increments; ++i) ++c;
                });
        }
    }

    CHECK(c.get() == threads * increments);
}

TEST_CASE("registry records its metrics", "[instrument][prometheus][registry][record]")
{
    auto& c = registry::register_metric(counter{"registry_record_test", "", {{"a", "b"}}});
    c += 7;

    string_writer<char> writer;
    auto                res = test::sync_wait(registry::record(writer));
    REQUIRE_FALSE(res.err);

    // NOTE: other tests register metrics too.
    auto out = writer.build();
    CHECK(out.find("# TYPE registry_record_test counter\nregistry_record_test{a=\"b\"} 7\n") != std::string::npos);
}
