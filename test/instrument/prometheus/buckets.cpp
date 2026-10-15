#include "instrument/prometheus/buckets.hpp"

#include <cmath>
#include <exception> // IWYU pragma: keep
#include <stdexcept>
#include <vector>

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

TEST_CASE("bucket helpers reject invalid buckets", "[instrument][prometheus][buckets]")
{
    // too few buckets (which used to write out of bounds, or divide by zero).
    CHECK_THROWS_AS(linear_buckets(0.1, 0.1, 0), std::invalid_argument);
    CHECK_THROWS_AS(linear_buckets(0.1, 0.1, 1), std::invalid_argument);
    CHECK_THROWS_AS(exponential_buckets(0.1, 2, 1), std::invalid_argument);
    CHECK_THROWS_AS(exponential_buckets_from_range(1, 100, 2), std::invalid_argument);

    // not increasing.
    CHECK_THROWS_AS(linear_buckets(0.1, 0, 3), std::invalid_argument);
    CHECK_THROWS_AS(linear_buckets(0.1, -1, 3), std::invalid_argument);
    CHECK_THROWS_AS(exponential_buckets(0.1, 1, 3), std::invalid_argument);
    CHECK_THROWS_AS(exponential_buckets(0, 2, 3), std::invalid_argument);
    CHECK_THROWS_AS(exponential_buckets_from_range(0, 100, 3), std::invalid_argument);
    CHECK_THROWS_AS(exponential_buckets_from_range(100, 1, 3), std::invalid_argument);
}

TEST_CASE("exponential buckets from range include the range's ends", "[instrument][prometheus][buckets]")
{
    auto actual = exponential_buckets_from_range(1, 100, 3);

    REQUIRE(actual.size() == 3);
    CHECK_THAT(actual[0], Catch::Matchers::WithinRel(1.0, 0.0001));
    CHECK_THAT(actual[1], Catch::Matchers::WithinRel(100.0, 0.0001));
    CHECK(std::isinf(actual[2]));
}
