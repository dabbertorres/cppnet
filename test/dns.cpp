#include "dns.hpp"

#include <exception> // IWYU pragma: keep
#include <string_view>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include "ip_addr.hpp"
#include "ipv6_addr.hpp"

TEST_CASE("dns_lookup", "[dns]")
{
    auto results = net::dns_lookup("google.com");

    int count = 0;

    for ([[maybe_unused]] const auto& _ : results) ++count;

    CHECK(count >= 1);
}

TEST_CASE("dns_lookup takes the host from a view of a longer string", "[dns]")
{
    // e.g. the host of "localhost:8080": getaddrinfo needs it null-terminated, so a view's data() isn't enough.
    constexpr std::string_view authority = "localhost:8080";

    int count = 0;
    for ([[maybe_unused]] const auto& _ : net::dns_lookup(authority.substr(0, authority.find(':')))) ++count;

    CHECK(count >= 1);
}

TEST_CASE("dns_lookup gives addresses in the right byte order", "[dns]")
{
    int count = 0;
    for (const auto& addr : net::dns_lookup("localhost"))
    {
        ++count;

        // a reversed address prints, and classifies, differently.
        CAPTURE(addr.to_string());
        if (addr.is_ipv4()) CHECK(addr.to_ipv4().is_loopback());
        if (addr.is_ipv6()) CHECK(addr.to_ipv6() == net::ipv6_addr::loopback());
    }

    CHECK(count >= 1);
}
