#include "dns.hpp"

#include <exception> // IWYU pragma: keep

#include <catch.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("dns_lookup", "[dns]")
{
    auto results = net::dns_lookup("google.com");

    int count = 0;

    for (auto _ : results) ++count;

    CHECK(count >= 1);
}
