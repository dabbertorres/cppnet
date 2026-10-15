#include "dns.hpp"

#include <exception> // IWYU pragma: keep
#include <string_view>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("dns_lookup", "[dns]")
{
    auto results = net::dns_lookup("google.com");

    int count = 0;

    for (auto _ : results) ++count;

    CHECK(count >= 1);
}

TEST_CASE("dns_lookup takes the host from a view of a longer string", "[dns]")
{
    // e.g. the host of "localhost:8080": getaddrinfo needs it null-terminated, so a view's data() isn't enough.
    constexpr std::string_view authority = "localhost:8080";

    int count = 0;
    for (auto _ : net::dns_lookup(authority.substr(0, authority.find(':')))) ++count;

    CHECK(count >= 1);
}
