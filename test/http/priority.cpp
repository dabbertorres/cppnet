#include "http/priority.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_tostring.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "http/structured_fields.hpp"

using net::http::parse_priority;
using net::http::priority;
using net::http::to_string;

using namespace std::string_view_literals;

namespace Catch
{

template<>
struct StringMaker<priority>
{
    static std::string convert(const priority& p)
    {
        return "{u=" + std::to_string(p.urgency) + (p.incremental ? ", i}" : "}");
    }
};

}

TEST_CASE("parse_priority reads RFC 9218's examples", "[http][priority]")
{
    struct test_case
    {
        std::string_view value;
        priority         want;
    };
    const auto tc = GENERATE(
        test_case{
            .value = "u=5",
            .want  = {.urgency = 5}
    },                           // section 4.1
        test_case{.value = "i", .want = {.incremental = true}}, // section 4.2
        test_case{.value = "u=0, i", .want = {.urgency = 0, .incremental = true}},
        test_case{.value = "u=5, i", .want = {.urgency = 5, .incremental = true}},
        test_case{.value = "i, u=5", .want = {.urgency = 5, .incremental = true}},
        test_case{.value = "i=?1", .want = {.incremental = true}},
        test_case{.value = "i=?0", .want = {}},
        test_case{.value = "u=3, i=?0", .want = {}},
        test_case{.value = "u=7", .want = {.urgency = 7}},
        test_case{.value = "u=1", .want = {.urgency = 1}});
    CAPTURE(tc.value);
    CHECK(parse_priority(tc.value) == tc.want);
}

TEST_CASE("parse_priority keeps base's parameters that the value doesn't have", "[http][priority]")
{
    const priority base{.urgency = 6, .incremental = true};
    CHECK(parse_priority("", base) == base);
    CHECK(parse_priority("u=1", base) == priority{.urgency = 1, .incremental = true});
    CHECK(parse_priority("i=?0", base) == priority{.urgency = 6, .incremental = false});
    CHECK(parse_priority("foo=bar", base) == base);

    // RFC 9218, section 8: a response's Priority merged over the request's.
    CHECK(parse_priority("i", parse_priority("u=5")) == priority{.urgency = 5, .incremental = true});
    CHECK(parse_priority("u=1", parse_priority("u=5, i")) == priority{.urgency = 1, .incremental = true});
}

TEST_CASE("parse_priority ignores values out of range or of other types", "[http][priority]")
{
    const priority base{.urgency = 6, .incremental = true};
    const auto     value = GENERATE("u=8"sv,
                                    "u=-1"sv,
                                    "u=255"sv,
                                    "u=263"sv, // 7 more than 256
                                    "u=999999999999999"sv,
                                    "u=1.0"sv,
                                    "u=\"1\""sv,
                                    "u=?1"sv,
                                    "u=a"sv,
                                    "u=:AQ==:"sv,
                                    "u=@1"sv,
                                    "u=%\"1\""sv,
                                    "u=(1)"sv,
                                    "u"sv, // Boolean true
                                    "i=1"sv,
                                    "i=0"sv,
                                    "i=\"?1\""sv,
                                    "i=(?0)"sv,
                                    "i=yes"sv,
                                    "i=0.0"sv);
    CAPTURE(value);
    CHECK(parse_priority(value, base) == base);
}

TEST_CASE("parse_priority takes a key's last member, then judges it", "[http][priority]")
{
    struct test_case
    {
        std::string_view value;
        priority         want;
    };
    const priority base{.urgency = 6, .incremental = false};
    const auto     tc = GENERATE(
        test_case{
            .value = "u=1, u=2",
            .want  = {.urgency = 2}
    },
        test_case{.value = "u=1, u=9", .want = {.urgency = 6}}, // u=9 is the value, and ignored
        test_case{.value = "u=9, u=1", .want = {.urgency = 1}},
        test_case{.value = "u=1, u=a", .want = {.urgency = 6}},
        test_case{.value = "i, i=?0", .want = {.urgency = 6}},
        test_case{.value = "i=?0, i", .want = {.urgency = 6, .incremental = true}},
        test_case{.value = "i, i=1", .want = {.urgency = 6}},
        test_case{.value = "u=2, i, u=4", .want = {.urgency = 4, .incremental = true}});
    CAPTURE(tc.value);
    CHECK(parse_priority(tc.value, base) == tc.want);
}

TEST_CASE("parse_priority ignores members it doesn't know, and parameters", "[http][priority]")
{
    struct test_case
    {
        std::string_view value;
        priority         want;
    };
    const auto tc = GENERATE(
        test_case{
            .value = "u=2, foo=bar",
            .want  = {.urgency = 2}
    },
        test_case{.value = R"(x=(1 "two" :AQ==:);p=?1, u=2, y=%"%c3%bc", z=@1, i, w=4.5)",
                  .want  = {.urgency = 2, .incremental = true}},
        test_case{.value = "u=2;foo=bar, i;x;y=1", .want = {.urgency = 2, .incremental = true}},
        test_case{.value = "u=2;u=5, i;i=?0", .want = {.urgency = 2, .incremental = true}},
        test_case{.value = "*u=1, u-=1, ui", .want = {}});
    CAPTURE(tc.value);
    CHECK(parse_priority(tc.value) == tc.want);
}

TEST_CASE("parse_priority leaves base whole for a value that isn't a dictionary", "[http][priority]")
{
    const priority base{.urgency = 1, .incremental = true};
    const auto     value = GENERATE("u=2, "sv,
                                    "u=2,,i"sv,
                                    "u=2 i"sv,
                                    "U=2"sv,
                                    "u =2"sv,
                                    "u= 2"sv,
                                    "u=2;"sv,
                                    "u=2, 1"sv,
                                    "u=2, i=?2"sv,
                                    "u=2, x=1234567890123456"sv, // an Integer of 16 digits
                                    "u=2, x=\"unterminated"sv,
                                    "u=2, x=(1 2"sv,
                                    "\tu=2"sv, // only spaces may lead (an HTTP field's value has none)
                                    "u=0 \t i"sv,
                                    "i="sv,
                                    ","sv,
                                    "u=2, x=%\"%c3\""sv); // a Display String that isn't UTF-8
    CAPTURE(value);
    CHECK(parse_priority(value, base) == base);
}

TEST_CASE("parse_priority allows spaces around the members, and tabs between them", "[http][priority]")
{
    CHECK(parse_priority("   u=2 ,\ti  ") == priority{.urgency = 2, .incremental = true});
    CHECK(parse_priority("u=2\t,\t i\t") == priority{.urgency = 2, .incremental = true});
}

TEST_CASE("parse_priority reads a field's lines joined", "[http][priority]")
{
    struct test_case
    {
        std::vector<std::string> lines;
        priority                 want;
    };
    const priority base{.urgency = 6, .incremental = true};
    const auto     tc = GENERATE_COPY(
        test_case{
            .lines = {"u=2", "i=?0"},
            .want  = {.urgency = 2}
    },
        test_case{.lines = {"u=2", "u=1"}, .want = {.urgency = 1, .incremental = true}},
        test_case{.lines = {"u=2, i=?0", "u=5"}, .want = {.urgency = 5}},
        test_case{.lines = {"u=2", ""}, .want = base}, // an empty member: none of it's taken
        test_case{.lines = {"u=2", "U=3"}, .want = base});
    std::string buffer;
    CHECK(parse_priority(net::http::sf::join_lines(tc.lines, buffer), base) == tc.want);
}

TEST_CASE("to_string leaves out the defaults, and parses back", "[http][priority]")
{
    CHECK(to_string(priority{}).empty());
    CHECK(to_string({.urgency = 5, .incremental = true}) == "u=5, i");
    CHECK(to_string({.incremental = true}) == "i");
    CHECK(to_string({.urgency = 0}) == "u=0");
    CHECK(to_string({.urgency = 7}) == "u=7");

    for (std::uint8_t u = 0; u <= 7; ++u)
    {
        for (const bool i : {false, true})
        {
            const priority p{.urgency = u, .incremental = i};
            CAPTURE(p);
            CHECK(parse_priority(to_string(p)) == p); // over the defaults, which it leaves out
        }
    }
}
