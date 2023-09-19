#include "instrument/prometheus/histogram.hpp"

#include <chrono>

#include <catch.hpp>

#include <catch2/catch_test_macros.hpp>

#include "io/string_writer.hpp"

using net::instrument::prometheus::histogram;
using net::io::string_writer;

TEST_CASE("histogram encodes without help nor labels", "[instrument][prometheus][histogram][encode]")
{
    auto h = histogram{"test"};

    // fill with some values
    h.observe(0.004);
    h.observe(0.009);
    h.observe(0.02);
    h.observe(0.04);
    h.observe(0.09);
    h.observe(0.24);
    h.observe(0.4);
    h.observe(0.9);
    h.observe(2.4);
    h.observe(4.9);
    h.observe(9.9);
    h.observe(100);

    string_writer<char> writer;
    auto                res = h.encode(writer);
    REQUIRE_FALSE(res.err);

    auto out = writer.build();

    REQUIRE(out == R"(# TYPE test histogram
test_bucket{le="0.005000",} 1
test_bucket{le="0.010000",} 2
test_bucket{le="0.025000",} 3
test_bucket{le="0.050000",} 4
test_bucket{le="0.100000",} 5
test_bucket{le="0.250000",} 6
test_bucket{le="0.500000",} 7
test_bucket{le="1.000000",} 8
test_bucket{le="2.500000",} 9
test_bucket{le="5.000000",} 10
test_bucket{le="10.000000",} 11
test_bucket{le="+Inf",} 12
test_sum 118.903000
test_count 12
)");
}

TEST_CASE("histogram encodes with help", "[instrument][prometheus][histogram][encode]")
{
    auto h = histogram{"test", "this is a help message"};

    // fill with some values
    h.observe(0.004);
    h.observe(0.009);
    h.observe(0.02);
    h.observe(0.04);
    h.observe(0.09);
    h.observe(0.24);
    h.observe(0.4);
    h.observe(0.9);
    h.observe(2.4);
    h.observe(4.9);
    h.observe(9.9);
    h.observe(100);

    string_writer<char> writer;
    auto                res = h.encode(writer);
    REQUIRE_FALSE(res.err);

    auto out = writer.build();

    REQUIRE(out == R"(# HELP test this is a help message
# TYPE test histogram
test_bucket{le="0.005000",} 1
test_bucket{le="0.010000",} 2
test_bucket{le="0.025000",} 3
test_bucket{le="0.050000",} 4
test_bucket{le="0.100000",} 5
test_bucket{le="0.250000",} 6
test_bucket{le="0.500000",} 7
test_bucket{le="1.000000",} 8
test_bucket{le="2.500000",} 9
test_bucket{le="5.000000",} 10
test_bucket{le="10.000000",} 11
test_bucket{le="+Inf",} 12
test_sum 118.903000
test_count 12
)");
}

TEST_CASE("histogram encodes with labels", "[instrument][prometheus][histogram][encode]")
{
    auto h = histogram{
        "test",
        "this is a help message",
        {{"foo", "bar"}, {"new", "true"}}
    };

    // fill with some values
    h.observe(0.004);
    h.observe(0.009);
    h.observe(0.02);
    h.observe(0.04);
    h.observe(0.09);
    h.observe(0.24);
    h.observe(0.4);
    h.observe(0.9);
    h.observe(2.4);
    h.observe(4.9);
    h.observe(9.9);
    h.observe(100);

    string_writer<char> writer;
    auto                res = h.encode(writer);
    REQUIRE_FALSE(res.err);

    auto out = writer.build();

    REQUIRE(out == R"(# HELP test this is a help message
# TYPE test histogram
test_bucket{le="0.005000",foo="bar",new="true",} 1
test_bucket{le="0.010000",foo="bar",new="true",} 2
test_bucket{le="0.025000",foo="bar",new="true",} 3
test_bucket{le="0.050000",foo="bar",new="true",} 4
test_bucket{le="0.100000",foo="bar",new="true",} 5
test_bucket{le="0.250000",foo="bar",new="true",} 6
test_bucket{le="0.500000",foo="bar",new="true",} 7
test_bucket{le="1.000000",foo="bar",new="true",} 8
test_bucket{le="2.500000",foo="bar",new="true",} 9
test_bucket{le="5.000000",foo="bar",new="true",} 10
test_bucket{le="10.000000",foo="bar",new="true",} 11
test_bucket{le="+Inf",foo="bar",new="true",} 12
test_sum{foo="bar",new="true",} 118.903000
test_count{foo="bar",new="true",} 12
)");
}
