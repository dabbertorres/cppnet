#include "sockaddr.hpp"

#include <cstring>
#include <system_error>

#include <sys/socket.h>
#include <sys/un.h>

#include <arpa/inet.h>
#include <catch2/catch_test_macros.hpp>
#include <netinet/in.h>

#include "ip_addr.hpp"
#include "ipv4_addr.hpp"
#include "ipv6_addr.hpp"

TEST_CASE("an IPv4 address and port round trip through a sockaddr_in", "[sockaddr]")
{
    const net::ip_addr addr{
        net::ipv4_addr{192, 0, 2, 1}
    };

    const auto converted = net::detail::to_sockaddr(addr, 8080);
    REQUIRE(converted.size == sizeof(sockaddr_in));
    REQUIRE(converted.family() == AF_INET);

    // in network byte order, as the OS reads them.
    sockaddr_in sin{};
    std::memcpy(&sin, &converted.storage, sizeof(sin));
    CHECK(sin.sin_port == htons(8080));

    in_addr expected{};
    REQUIRE(::inet_pton(AF_INET, "192.0.2.1", &expected) == 1);
    CHECK(sin.sin_addr.s_addr == expected.s_addr);

    auto back = net::detail::from_sockaddr(converted);
    REQUIRE(back.has_value());
    CHECK(back->addr == addr);
    CHECK(back->port == 8080);
}

TEST_CASE("an IPv6 address and port round trip through a sockaddr_in6", "[sockaddr]")
{
    const net::ip_addr addr{
        net::ipv6_addr{0x2001, 0xdb8, 0, 0, 0, 0, 0x1234, 0x5678}
    };

    const auto converted = net::detail::to_sockaddr(addr, 443);
    REQUIRE(converted.size == sizeof(sockaddr_in6));
    REQUIRE(converted.family() == AF_INET6);

    sockaddr_in6 sin6{};
    std::memcpy(&sin6, &converted.storage, sizeof(sin6));
    CHECK(sin6.sin6_port == htons(443));
    CHECK(sin6.sin6_scope_id == 0);
    CHECK(sin6.sin6_flowinfo == 0);

    in6_addr expected{};
    REQUIRE(::inet_pton(AF_INET6, "2001:db8::1234:5678", &expected) == 1);
    CHECK(std::memcmp(&sin6.sin6_addr, &expected, sizeof(expected)) == 0);

    auto back = net::detail::from_sockaddr(converted);
    REQUIRE(back.has_value());
    CHECK(back->addr == addr);
    CHECK(back->port == 443);
}

TEST_CASE("only IPv4 and IPv6 socket addresses convert", "[sockaddr]")
{
    SECTION("another family")
    {
        sockaddr_un local{};
        local.sun_family = AF_UNIX;

        auto converted = net::detail::from_sockaddr(reinterpret_cast<const sockaddr*>(&local), sizeof(local));
        REQUIRE_FALSE(converted.has_value());
        CHECK(converted.error() == std::errc::address_family_not_supported);
    }

    SECTION("too short for its family")
    {
        const auto addr = net::detail::to_sockaddr(net::ipv6_addr::loopback(), 80);

        auto converted = net::detail::from_sockaddr(addr.get(), sizeof(sockaddr_in));
        REQUIRE_FALSE(converted.has_value());
        CHECK(converted.error() == std::errc::invalid_argument);
    }
}
