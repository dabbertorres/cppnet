#include "instrument/prometheus/registry.hpp"

#include <exception> // IWYU pragma: keep
#include <stdexcept>
#include <string>
#include <system_error>

#include <catch2/catch_test_macros.hpp>

#include "coro/sync_wait.hpp"
#include "instrument/prometheus/counter.hpp"
#include "instrument/prometheus/gauge.hpp"
#include "instrument/prometheus/histogram.hpp"
#include "io/string_writer.hpp"

using net::instrument::prometheus::counter;
using net::instrument::prometheus::gauge;
using net::instrument::prometheus::histogram;
using net::instrument::prometheus::metric;
using net::instrument::prometheus::metric_labels;
using net::instrument::prometheus::registry;
using net::instrument::prometheus::variant_has;

// NOTE: the registry is global, so each test uses its own metric names.

static_assert(variant_has<counter, metric>);
static_assert(variant_has<histogram, metric>);
static_assert(!variant_has<int, metric>);
static_assert(!variant_has<counter&, metric>);

namespace
{

std::string record()
{
    net::io::string_writer<char> writer;
    auto                         res = test::sync_wait(registry::record(writer));
    REQUIRE_FALSE(res.err);
    return writer.build();
}

}

TEST_CASE("can register counter", "[instrument][prometheus][registry][register]")
{
    auto& c = registry::register_metric(counter{"my_counter", "", {{.key = "foo", .val = "bar"}}});

    CHECK(c.name == "my_counter");
    CHECK(c.help.empty());
    CHECK(c.labels
          == metric_labels{
              {"foo", "bar"}
    });

    // it's the registered metric.
    auto found = registry::get_metric<counter>("my_counter",
                                               {
                                                   {.key = "foo", .val = "bar"}
    });
    REQUIRE(found.has_value());
    CHECK(&found->get() == &c);
}

TEST_CASE("can register gauge", "[instrument][prometheus][registry][register]")
{
    auto& g = registry::register_metric(gauge{"my_gauge", "", {{.key = "foo", .val = "bar"}}});

    CHECK(g.name == "my_gauge");
    CHECK(&registry::get_metric<gauge>("my_gauge",
                                       {
                                           {"foo", "bar"}
    })
               ->get()
          == &g);
}

TEST_CASE("can register histogram", "[instrument][prometheus][registry][register]")
{
    auto& h = registry::register_metric(histogram{"my_histogram", "", {{.key = "foo", .val = "bar"}}});

    CHECK(h.name == "my_histogram");
    CHECK(&registry::get_metric<histogram>("my_histogram",
                                           {
                                               {"foo", "bar"}
    })
               ->get()
          == &h);
}

TEST_CASE("registering a metric again returns the first", "[instrument][prometheus][registry][register]")
{
    auto& first = registry::register_metric(counter{
        "registry_again",
        "",
        {{.key = "a", .val = "1"}, {.key = "b", .val = "2"}}
    });
    ++first;

    // labels in another order are the same metric.
    auto& again = registry::register_metric(counter{
        "registry_again",
        "",
        {{.key = "b", .val = "2"}, {.key = "a", .val = "1"}}
    });
    CHECK(&again == &first);
    CHECK(again.get() == 1);

    CHECK(registry::get_metric<counter>("registry_again",
                                        {
                                            {"b", "2"},
                                            {"a", "1"}
    })
              .has_value());
}

TEST_CASE("a metric family has one type and help", "[instrument][prometheus][registry][register]")
{
    registry::register_metric(counter{"registry_family", "help", {{.key = "a", .val = "1"}}});

    // another type, with the same or other labels.
    CHECK_THROWS_AS(registry::register_metric(gauge{"registry_family", "help", {{"a", "1"}}}), std::invalid_argument);
    CHECK_THROWS_AS(registry::register_metric(gauge{"registry_family", "help", {{"a", "2"}}}), std::invalid_argument);

    // other help.
    CHECK_THROWS_AS(registry::register_metric(counter{"registry_family", "other", {{"a", "2"}}}),
                    std::invalid_argument);

    // asking for it as another type finds nothing.
    CHECK_FALSE(registry::get_metric<gauge>("registry_family",
                                            {
                                                {"a", "1"}
    })
                    .has_value());
}

TEST_CASE("registry records each family once", "[instrument][prometheus][registry][record]")
{
    auto& b = registry::register_metric(counter{"registry_record", "Things.", {{.key = "kind", .val = "b"}}});
    auto& a = registry::register_metric(counter{"registry_record", "Things.", {{.key = "kind", .val = "a"}}});
    b += 2;
    a += 1;

    // one HELP and TYPE, then the family's samples, in label order.
    auto out = record();
    CHECK(out.contains(R"(# HELP registry_record Things.
# TYPE registry_record counter
registry_record{kind="a"} 1
registry_record{kind="b"} 2
)"));
}
