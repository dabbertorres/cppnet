#include "instrument/prometheus/registry.hpp"

#include <catch.hpp>

#include <catch2/catch_test_macros.hpp>

#include "instrument/prometheus/counter.hpp"

using net::instrument::prometheus::counter;
using net::instrument::prometheus::gauge;
using net::instrument::prometheus::histogram;
using net::instrument::prometheus::metric_labels;
using net::instrument::prometheus::registry;

TEST_CASE("can register counter", "[instrument][prometheus][registry][register]")
{
    auto c = registry::register_metric(counter{
        "my_counter",
        "",
        {{"foo", "bar"}},
    });

    REQUIRE(c.name == "my_counter");
    REQUIRE(c.help.empty());
    REQUIRE(c.labels
            == metric_labels{
                {"foo", "bar"}
    });
}

TEST_CASE("can register gauge", "[instrument][prometheus][registry][register]")
{
    auto c = registry::register_metric(gauge{
        "my_gauge",
        "",
        {{"foo", "bar"}},
    });

    REQUIRE(c.name == "my_gauge");
    REQUIRE(c.help.empty());
    REQUIRE(c.labels
            == metric_labels{
                {"foo", "bar"}
    });
}

TEST_CASE("can register histogram", "[instrument][prometheus][registry][register]")
{
    auto c = registry::register_metric(histogram{
        "my_histogram",
        "",
        {{"foo", "bar"}},
    });

    REQUIRE(c.name == "my_histogram");
    REQUIRE(c.help.empty());
    REQUIRE(c.labels
            == metric_labels{
                {"foo", "bar"}
    });
}
