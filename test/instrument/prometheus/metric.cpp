#include "instrument/prometheus/metric.hpp"

#include <cstddef>
#include <exception> // IWYU pragma: keep
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <catch.hpp>

#include <catch2/catch_test_macros.hpp>

#include "coro/sync_wait.hpp"
#include "instrument/prometheus/counter.hpp"
#include "instrument/prometheus/gauge.hpp"
#include "instrument/prometheus/histogram.hpp"
#include "io/string_writer.hpp"

using net::instrument::prometheus::counter;
using net::instrument::prometheus::gauge;
using net::instrument::prometheus::histogram;
using net::instrument::prometheus::metric_labels;
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

TEST_CASE("metric names and labels are validated", "[instrument][prometheus][metric]")
{
    CHECK_NOTHROW(counter{"ok_name:total"});
    CHECK_NOTHROW(counter{"_ok", "", {{"_ok_label1", "x"}}});

    CHECK_THROWS_AS(counter{""}, std::invalid_argument);
    CHECK_THROWS_AS(counter{"1st"}, std::invalid_argument);
    CHECK_THROWS_AS(counter{"has-dash"}, std::invalid_argument);
    CHECK_THROWS_AS(counter{"has space"}, std::invalid_argument);

    CHECK_THROWS_AS((counter{"name", "", {{"", "x"}}}), std::invalid_argument);
    CHECK_THROWS_AS((counter{"name", "", {{"a:b", "x"}}}), std::invalid_argument);
    CHECK_THROWS_AS((counter{"name", "", {{"__reserved", "x"}}}), std::invalid_argument);
    CHECK_THROWS_AS((counter{"name", "", {{"a", "x"}, {"a", "y"}}}), std::invalid_argument);
}

TEST_CASE("metric labels are sorted", "[instrument][prometheus][metric]")
{
    auto c = counter{"name", "", {{"b", "2"}, {"a", "1"}}};
    CHECK(c.labels == metric_labels{{"a", "1"}, {"b", "2"}});
}

TEST_CASE("derive_child adds labels", "[instrument][prometheus][metric]")
{
    auto parent = histogram{"latency", "Latency.", {{"a", "1"}}, {0.5, 1.0}};
    parent.observe(0.1);

    // a label it already has isn't replaced.
    auto child = derive_child(parent, {{"b", "2"}, {"a", "other"}});

    CHECK(child.name == "latency");
    CHECK(child.help == "Latency.");
    CHECK(child.labels == metric_labels{{"a", "1"}, {"b", "2"}});
    CHECK(child.buckets() == parent.buckets());

    // and it has nothing observed.
    CHECK(encode(child).find(R"(latency_count{a="1",b="2"} 0)") != std::string::npos);

    auto c = derive_child(counter{"total"}, {{"x", "y"}});
    CHECK(c.labels == metric_labels{{"x", "y"}});
}
