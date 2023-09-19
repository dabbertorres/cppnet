#include "instrument/prometheus/buckets.hpp"

#include <cmath>
#include <limits>
#include <vector>

#include <catch.hpp>

#include <catch2/catch_test_macros.hpp>

#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using net::instrument::prometheus::exponential_buckets;
using net::instrument::prometheus::exponential_buckets_from_range;
using net::instrument::prometheus::linear_buckets;

TEST_CASE("linear buckets", "[instrument][prometheus][buckets]")
{
    auto actual = linear_buckets(0.1, 0.1, 6);

    REQUIRE(actual.size() == 6);
    CHECK_THAT(actual[0], Catch::Matchers::WithinRel(0.1, 0.0001));
    CHECK_THAT(actual[1], Catch::Matchers::WithinRel(0.2, 0.0001));
    CHECK_THAT(actual[2], Catch::Matchers::WithinRel(0.3, 0.0001));
    CHECK_THAT(actual[3], Catch::Matchers::WithinRel(0.4, 0.0001));
    CHECK_THAT(actual[4], Catch::Matchers::WithinRel(0.5, 0.0001));
    CHECK(std::isinf(actual[5]));
}

TEST_CASE("exponential buckets from factor", "[instrument][prometheus][buckets]")
{
    auto actual = exponential_buckets(0.1, 2, 6);

    REQUIRE(actual.size() == 6);
    CHECK_THAT(actual[0], Catch::Matchers::WithinRel(0.1, 0.0001));
    CHECK_THAT(actual[1], Catch::Matchers::WithinRel(0.2, 0.0001));
    CHECK_THAT(actual[2], Catch::Matchers::WithinRel(0.4, 0.0001));
    CHECK_THAT(actual[3], Catch::Matchers::WithinRel(0.8, 0.0001));
    CHECK_THAT(actual[4], Catch::Matchers::WithinRel(1.6, 0.0001));
    CHECK(std::isinf(actual[5]));
}

TEST_CASE("exponential buckets from range", "[instrument][prometheus][buckets]")
{
    auto actual = exponential_buckets_from_range(0.1, 1.0, 6);

    REQUIRE(actual.size() == 6);
    CHECK_THAT(actual[0], Catch::Matchers::WithinRel(0.1, 0.0001));
    CHECK_THAT(actual[1], Catch::Matchers::WithinRel(0.177827941, 0.0001));
    CHECK_THAT(actual[2], Catch::Matchers::WithinRel(0.316227766, 0.0001));
    CHECK_THAT(actual[3], Catch::Matchers::WithinRel(0.5623413252, 0.0001));
    CHECK_THAT(actual[4], Catch::Matchers::WithinRel(1.0, 0.0001));
    CHECK(std::isinf(actual[5]));
}
