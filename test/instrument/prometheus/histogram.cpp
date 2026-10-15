#include "instrument/prometheus/histogram.hpp"

#include <chrono>
#include <cstddef>
#include <exception> // IWYU pragma: keep
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "coro/sync_wait.hpp"
#include "instrument/prometheus/metric.hpp"
#include "io/io.hpp"
#include "io/string_writer.hpp"

using namespace std::chrono_literals;

using net::instrument::prometheus::histogram;
using net::io::string_writer;

namespace
{

std::string encode(const histogram& h)
{
    string_writer<char> writer;
    auto                res = test::sync_wait(h.encode(writer));
    REQUIRE_FALSE(res.err);

    auto out = writer.build();
    CHECK(res.count == out.size());
    return out;
}

// observe_one_per_bucket observes one value in each of the default buckets: some (0.5 and 1) exactly on a bucket's
// bound, which belong in that bucket. They're exact in binary, so their sum is too.
void observe_one_per_bucket(histogram& h)
{
    for (double v : {0.00390625, 0.0078125, 0.015625, 0.03125, 0.0625, 0.125, 0.5, 1.0, 2.0, 4.0, 8.0, 64.0})
    {
        h.observe(v);
    }
}

}

TEST_CASE("histogram encodes without help nor labels", "[instrument][prometheus][histogram][encode]")
{
    auto h = histogram{"test"};
    observe_one_per_bucket(h);

    CHECK(encode(h) == R"(# TYPE test histogram
test_bucket{le="0.005"} 1
test_bucket{le="0.01"} 2
test_bucket{le="0.025"} 3
test_bucket{le="0.05"} 4
test_bucket{le="0.1"} 5
test_bucket{le="0.25"} 6
test_bucket{le="0.5"} 7
test_bucket{le="1"} 8
test_bucket{le="2.5"} 9
test_bucket{le="5"} 10
test_bucket{le="10"} 11
test_bucket{le="+Inf"} 12
test_sum 79.74609375
test_count 12
)");
}

TEST_CASE("histogram encodes with help", "[instrument][prometheus][histogram][encode]")
{
    // backslashes and newlines are escaped.
    auto h = histogram{"test", "this is a help message\nwith a \\ in it", {}, {1.0}};
    h.observe(0.5);

    CHECK(encode(h) == R"(# HELP test this is a help message\nwith a \\ in it
# TYPE test histogram
test_bucket{le="1"} 1
test_bucket{le="+Inf"} 1
test_sum 0.5
test_count 1
)");
}

TEST_CASE("histogram encodes with labels", "[instrument][prometheus][histogram][encode]")
{
    // label values escape backslashes, newlines and double quotes.
    auto h = histogram{
        "test",
        "this is a help message",
        {{"foo", "bar"}, {"quoted", "\"a\\b\"\n"}},
        {1.0}
    };
    h.observe(2.0);

    CHECK(encode(h) == R"(# HELP test this is a help message
# TYPE test histogram
test_bucket{le="1",foo="bar",quoted="\"a\\b\"\n"} 0
test_bucket{le="+Inf",foo="bar",quoted="\"a\\b\"\n"} 1
test_sum{foo="bar",quoted="\"a\\b\"\n"} 2
test_count{foo="bar",quoted="\"a\\b\"\n"} 1
)");
}

TEST_CASE("histogram encodes bounds and values exactly", "[instrument][prometheus][histogram][encode]")
{
    // small bounds don't lose their digits (e.g. to 0.000000), and infinite and NaN values are spelled as Prometheus
    // expects.
    auto h = histogram{
        "test",
        "",
        {},
        {-1.5, 1e-7, 0.1}
    };
    h.observe(-std::numeric_limits<double>::infinity());
    h.observe(1e-7);
    h.observe(std::numeric_limits<double>::infinity());

    CHECK(encode(h) == R"(# TYPE test histogram
test_bucket{le="-1.5"} 1
test_bucket{le="1e-07"} 2
test_bucket{le="0.1"} 2
test_bucket{le="+Inf"} 3
test_sum NaN
test_count 3
)");
}

TEST_CASE("histogram counts NaN only in the +Inf bucket", "[instrument][prometheus][histogram][observe]")
{
    auto h = histogram{"test", "", {}, {1.0}};
    h.observe(std::numeric_limits<double>::quiet_NaN());

    CHECK(encode(h) == R"(# TYPE test histogram
test_bucket{le="1"} 0
test_bucket{le="+Inf"} 1
test_sum NaN
test_count 1
)");
}

TEST_CASE("histogram buckets must be increasing", "[instrument][prometheus][histogram][buckets]")
{
    CHECK_THROWS_AS((histogram{
                        "test",
                        "",
                        {},
                        {1.0, 1.0}
    }),
                    std::invalid_argument);
    CHECK_THROWS_AS((histogram{
                        "test",
                        "",
                        {},
                        {2.0, 1.0}
    }),
                    std::invalid_argument);
    CHECK_THROWS_AS((histogram{
                        "test",
                        "",
                        {},
                        {1.0, std::numeric_limits<double>::quiet_NaN()}
    }),
                    std::invalid_argument);
    CHECK_THROWS_AS((histogram{
                        "test",
                        "",
                        {},
                        {std::numeric_limits<double>::infinity(), 1.0}
    }),
                    std::invalid_argument);

    // "le" is the buckets' label.
    CHECK_THROWS_AS((histogram{"test", "", {{"le", "1"}}}), std::invalid_argument);

    // without any, there's only the +Inf bucket.
    auto h = histogram{"test", "", {}, {}};
    h.observe(1.0);
    CHECK(encode(h) == R"(# TYPE test histogram
test_bucket{le="+Inf"} 1
test_sum 1
test_count 1
)");
}

TEST_CASE("histogram observations from many threads aren't lost", "[instrument][prometheus][histogram][observe]")
{
    constexpr std::size_t threads      = 8;
    constexpr std::size_t observations = 50'000;

    auto h = histogram{"test", "", {}, {0.5}};

    {
        std::vector<std::jthread> workers;
        for (std::size_t t = 0; t < threads; ++t)
        {
            workers.emplace_back(
                [&h]
                {
                    for (std::size_t i = 0; i < observations; ++i) h.observe(1.0);
                });
        }
    }

    CHECK(encode(h) == R"(# TYPE test histogram
test_bucket{le="0.5"} 0
test_bucket{le="+Inf"} 400000
test_sum 400000
test_count 400000
)");
}

TEST_CASE("histogram tracks fractions of a second", "[instrument][prometheus][histogram][track]")
{
    auto h = histogram{
        "test",
        "",
        {},
        {0.001, 60.0}
    };

    {
        auto tracker = h.track();
        std::this_thread::sleep_for(5ms);
    }

    // it took at least 5ms (rather than 0s, as whole seconds).
    auto out = encode(h);
    CHECK(out.find("test_bucket{le=\"0.001\"} 0\n") != std::string::npos);
    CHECK(out.find("test_bucket{le=\"60\"} 1\n") != std::string::npos);
    CHECK(out.find("test_count 1\n") != std::string::npos);
}
