#include "ip_addr.hpp"

#include <cstdint>
#include <exception> // IWYU pragma: keep
#include <string>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_tostring.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "ipv4_addr.hpp"
#include "ipv6_addr.hpp"

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

    CHECK(result.value() == ipv4_addr(127, 0, 0, 1));
    CHECK(static_cast<std::uint32_t>(result.value()) == 0x7f00'0001);
    CHECK(result.value().is_loopback());
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

    CHECK(result.value() == ipv6_addr(1, 0xf0, 0xb0, 0, 5, 0, 0, 1));
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

    CHECK(result.value() == ipv4_addr(127, 0, 0, 1));
}

TEST_CASE("ipv6", "[ip_addr][parse]")
{
    auto result = ip_addr::parse("01:f0:b0:0:5:0:0:1");
    REQUIRE(result.has_value());

    CHECK(result.value() == ipv6_addr(1, 0xf0, 0xb0, 0, 5, 0, 0, 1));
}

TEST_CASE("all single digits", "[ipv4_addr][to_string]")
{
    ipv4_addr addr(1, 1, 1, 1);

    CHECK(addr.to_string() == "1.1.1.1");
}

TEST_CASE("all multi digits", "[ipv4_addr][to_string]")
{
    ipv4_addr addr(255, 254, 253, 252);

    CHECK(addr.to_string() == "255.254.253.252");
}

TEST_CASE("mixed digits", "[ipv4_addr][to_string]")
{
    ipv4_addr addr(255, 1, 253, 3);

    CHECK(addr.to_string() == "255.1.253.3");
}

TEST_CASE("all single digits", "[ipv6_addr][to_string]")
{
    ipv6_addr addr(1, 1, 1, 1, 1, 1, 1, 1);

    CHECK(addr.to_string() == "1:1:1:1:1:1:1:1");
}

TEST_CASE("all multi digits", "[ipv6_addr][to_string]")
{
    ipv6_addr addr(255, 254, 253, 252, 251, 250, 249, 248, 247, 246, 245, 244, 243, 242, 241, 240);

    CHECK(addr.to_string() == "fffe:fdfc:fbfa:f9f8:f7f6:f5f4:f3f2:f1f0");
}

TEST_CASE("mixed digits", "[ipv6_addr][to_string]")
{
    ipv6_addr addr(255, 0, 254, 1, 253, 2, 252, 3, 251, 4, 250, 5, 249, 6, 248, 7);

    CHECK(addr.to_string() == "ff00:fe01:fd02:fc03:fb04:fa05:f906:f807");
}

TEST_CASE("parse rejects malformed addresses", "[ipv4_addr][ipv6_addr][parse]")
{
    // too many parts used to be written before the start of the parsed values.
    for (const auto* str : {"1.2.3.4.5", "1.2.3", "1.2..4", "1.2.3.4.", "1a.2.3.4", "1.2.3.256", "-1.2.3.4", ""})
    {
        CAPTURE(str);
        CHECK_FALSE(ipv4_addr::parse(str).has_value());
    }

    for (const auto* str : {"1:2:3:4:5:6:7:8:9",
                            "1:2:3:4:5:6:7",
                            "1:2:3:4:5:6:7:8g",
                            "1:2:3:4:5:6:7:10000",
                            "1:2:3:4:5:6:7:00001",
                            "1::2::3",
                            ":::",
                            "1:::2",
                            ":1:2:3:4:5:6:7",
                            "1:2:3:4:5:6:7:",
                            "1:2:3:4::5:6:7:8",
                            "1.2.3.4::",
                            "::1.2.3",
                            "::1.2.3.4:5",
                            "1:2:3:4:5:6:7:1.2.3.4",
                            ""})
    {
        CAPTURE(str);
        CHECK_FALSE(ipv6_addr::parse(str).has_value());
    }
}

TEST_CASE("parse shortened IPv6 addresses", "[ipv6_addr][parse]")
{
    struct test_case
    {
        const char* str;
        ipv6_addr   want;
    };

    auto tc = GENERATE(
        test_case{.str = "::", .want = ipv6_addr{}},
        test_case{.str = "::1", .want = ipv6_addr::loopback()},
        test_case{.str = "1::", .want = ipv6_addr(1, 0, 0, 0, 0, 0, 0, 0)},
        test_case{.str = "2001:db8::ff00:42:8329", .want = ipv6_addr(0x2001, 0xdb8, 0, 0, 0, 0xff00, 0x42, 0x8329)},
        test_case{.str = "1:2:3:4:5:6::8", .want = ipv6_addr(1, 2, 3, 4, 5, 6, 0, 8)},
        test_case{.str = "::ffff:192.0.2.128", .want = ipv6_addr(0, 0, 0, 0, 0, 0xffff, 0xc000, 0x0280)},
        test_case{.str = "1:2:3:4:5:6:192.0.2.128", .want = ipv6_addr(1, 2, 3, 4, 5, 6, 0xc000, 0x0280)},
        test_case{.str = "FE80::ABCD", .want = ipv6_addr(0xfe80, 0, 0, 0, 0, 0, 0, 0xabcd)});

    CAPTURE(tc.str);
    auto result = ipv6_addr::parse(tc.str);
    REQUIRE(result.has_value());
    CHECK(result.value() == tc.want);
}

TEST_CASE("IPv6 addresses are formatted as RFC 5952 recommends", "[ipv6_addr][to_string]")
{
    struct test_case
    {
        ipv6_addr   addr;
        const char* want;
    };

    auto tc = GENERATE(
        test_case{.addr = ipv6_addr{}, .want = "::"},
        test_case{.addr = ipv6_addr::loopback(), .want = "::1"},
        test_case{.addr = ipv6_addr(1, 0, 0, 0, 0, 0, 0, 0), .want = "1::"},
        test_case{.addr = ipv6_addr(0x2001, 0xdb8, 0, 0, 0, 0xff00, 0x42, 0x8329), .want = "2001:db8::ff00:42:8329"},
        // a single zero group isn't shortened.
        test_case{.addr = ipv6_addr(0x2001, 0xdb8, 0, 1, 1, 1, 1, 1), .want = "2001:db8:0:1:1:1:1:1"},
        // the longest run is shortened, or the first of the longest.
        test_case{.addr = ipv6_addr(0x2001, 0, 0, 1, 0, 0, 0, 1), .want = "2001:0:0:1::1"},
        test_case{.addr = ipv6_addr(0x2001, 0xdb8, 0, 0, 1, 0, 0, 1), .want = "2001:db8::1:0:0:1"},
        test_case{.addr = ipv6_addr(0xABCD, 0x0EF0, 1, 2, 3, 4, 5, 6), .want = "abcd:ef0:1:2:3:4:5:6"});

    CAPTURE(tc.want);
    CHECK(tc.addr.to_string() == tc.want);
    CHECK(ipv6_addr::parse(tc.want) == tc.addr);
}

TEST_CASE("IPv4 address classification", "[ipv4_addr]")
{
    CHECK(ipv4_addr::parse("127.1.2.3")->is_loopback());
    CHECK_FALSE(ipv4_addr::parse("128.0.0.1")->is_loopback());

    CHECK(ipv4_addr::parse("10.1.2.3")->is_private_network());
    CHECK(ipv4_addr::parse("172.16.0.1")->is_private_network());
    CHECK(ipv4_addr::parse("172.31.255.255")->is_private_network());
    CHECK(ipv4_addr::parse("192.168.1.1")->is_private_network());
    CHECK_FALSE(ipv4_addr::parse("172.32.0.1")->is_private_network());
    CHECK_FALSE(ipv4_addr::parse("192.169.168.1")->is_private_network());
}

TEST_CASE("IPv6 address classification", "[ipv6_addr]")
{
    CHECK(ipv6_addr::parse("::1")->is_loopback());
    CHECK_FALSE(ipv6_addr::parse("::")->is_loopback());
    CHECK_FALSE(ipv6_addr::parse("::1:0")->is_loopback());

    // fc00::/7
    CHECK(ipv6_addr::parse("fc00::1")->is_unique_local());
    CHECK(ipv6_addr::parse("fdff:ffff::1")->is_unique_local());
    CHECK_FALSE(ipv6_addr::parse("fbff::1")->is_unique_local());
    CHECK_FALSE(ipv6_addr::parse("fe80::1")->is_unique_local());
    CHECK_FALSE(ipv6_addr::parse("ff02::1")->is_unique_local());

    // fe80::/10, whatever the 54 bits after it (zero in the addresses hosts form)
    CHECK(ipv6_addr::parse("fe80::1")->is_link_local());
    CHECK(ipv6_addr::parse("fe80::ffff:ffff:ffff:ffff")->is_link_local());
    CHECK(ipv6_addr::parse("fe80:0:0:1::1")->is_link_local());
    CHECK(ipv6_addr::parse("febf:ffff::1")->is_link_local());
    CHECK_FALSE(ipv6_addr::parse("fe7f::1")->is_link_local());
    CHECK_FALSE(ipv6_addr::parse("fec0::1")->is_link_local());
    CHECK_FALSE(ipv6_addr::parse("2001:db8::1")->is_link_local());

    // ff00::/8
    CHECK(ipv6_addr::parse("ff02::1")->is_multicast());
    CHECK(ipv6_addr::parse("ff00::")->is_multicast());
    CHECK_FALSE(ipv6_addr::parse("fe80::1")->is_multicast());
}

TEST_CASE("an IPv6 address splits into its subnet prefix and interface identifier", "[ipv6_addr]")
{
    const auto addr = ipv6_addr::parse("2001:db8:1:2:3:4:5:6");
    REQUIRE(addr.has_value());
    CHECK(addr->subnet_prefix() == 0x2001'0db8'0001'0002);
    CHECK(addr->interface_identifier() == 0x0003'0004'0005'0006);
}
