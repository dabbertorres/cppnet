#include "ip_addr.hpp"

#include <catch.hpp>

#include <catch2/catch_test_macros.hpp>

using net::ip_addr;
using net::ipv4_addr;
using net::ipv6_addr;

namespace Catch
{

template<>
struct StringMaker<ip_addr>
{
    static std::string convert(const ip_addr& value) { return value.to_string(); }
};

template<>
struct StringMaker<ipv4_addr>
{
    static std::string convert(const ipv4_addr& value) { return value.to_string(); }
};

template<>
struct StringMaker<ipv6_addr>
{
    static std::string convert(const ipv6_addr& value) { return value.to_string(); }
};

}

TEST_CASE("parse single digits", "[ipv4_addr][parse]")
{
    auto result = ipv4_addr::parse("1.1.1.1");
    REQUIRE(result.has_value());

    CHECK(result.value() == ipv4_addr(1, 1, 1, 1));
}

TEST_CASE("parse multi digit", "[ipv4_addr][parse]")
{
    auto result = ipv4_addr::parse("127.0.0.1");
    REQUIRE(result.has_value());

    CHECK(result.value() == ipv4_addr(1, 0, 0, 127));
}

TEST_CASE("parse all multi-digit", "[ipv4_addr][parse]")
{
    auto result = ipv4_addr::parse("255.255.255.255");
    REQUIRE(result.has_value());

    CHECK(result.value() == ipv4_addr(255, 255, 255, 255));
}

TEST_CASE("parse single digits", "[ipv6_addr][parse]")
{
    auto result = ipv6_addr::parse("1:1:1:1:1:1:1:1");
    REQUIRE(result.has_value());

    CHECK(result.value() == ipv6_addr(1, 1, 1, 1, 1, 1, 1, 1));
}

TEST_CASE("parse mixed digit", "[ipv6_addr][parse]")
{
    auto result = ipv6_addr::parse("01:f0:b0:0:5:0:0:1");
    REQUIRE(result.has_value());

    CHECK(result.value() == ipv6_addr(1, 0, 0, 5, 0, 0xb0, 0xf0, 1));
}

TEST_CASE("parse all multi-digit", "[ipv6_addr][parse]")
{
    auto result = ipv6_addr::parse("ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff");
    REQUIRE(result.has_value());

    CHECK(result.value() == ipv6_addr(0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff));
}

TEST_CASE("ipv4", "[ip_addr][parse]")
{
    auto result = ip_addr::parse("127.0.0.1");
    REQUIRE(result.has_value());

    CHECK(result.value() == ipv4_addr(1, 0, 0, 127));
}

TEST_CASE("ipv6", "[ip_addr][parse]")
{
    auto result = ip_addr::parse("01:f0:b0:0:5:0:0:1");
    REQUIRE(result.has_value());

    CHECK(result.value() == ipv6_addr(1, 0, 0, 5, 0, 0xb0, 0xf0, 1));
}

TEST_CASE("all single digits", "[ipv4_addr][to_string]")
{
    ipv4_addr addr(1, 1, 1, 1);

    CHECK(addr.to_string() == "1.1.1.1");
}

TEST_CASE("all multi digits", "[ipv4_addr][to_string]")
{
    ipv4_addr addr(255, 254, 253, 252);

    CHECK(addr.to_string() == "252.253.254.255");
}

TEST_CASE("mixed digits", "[ipv4_addr][to_string]")
{
    ipv4_addr addr(255, 1, 253, 3);

    CHECK(addr.to_string() == "3.253.1.255");
}

TEST_CASE("all single digits", "[ipv6_addr][to_string]")
{
    ipv6_addr addr(1, 1, 1, 1, 1, 1, 1, 1);

    CHECK(addr.to_string() == "1:1:1:1:1:1:1:1");
}

TEST_CASE("all multi digits", "[ipv6_addr][to_string]")
{
    ipv6_addr addr(255, 254, 253, 252, 251, 250, 249, 248, 247, 246, 245, 244, 243, 242, 241, 240);

    CHECK(addr.to_string() == "f1f0:f3f2:f5f4:f7f6:f9f8:fbfa:fdfc:fffe");
}

TEST_CASE("mixed digits", "[ipv6_addr][to_string]")
{
    ipv6_addr addr(255, 0, 254, 1, 253, 2, 252, 3, 251, 4, 250, 5, 249, 6, 248, 7);

    CHECK(addr.to_string() == "f807:f906:fa05:fb04:fc03:fd02:fe01:ff00");
}
