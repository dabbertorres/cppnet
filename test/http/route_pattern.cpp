#include "http/route_pattern.hpp"

#include <exception> // IWYU pragma: keep
#include <string>
#include <string_view>
#include <system_error>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

using namespace std::string_view_literals;

using net::http::check_route_pattern;
using net::http::route_error;
using net::http::route_pattern;

// the checks run at compile time, which is what makes a bad pattern given as a string literal fail to compile.
static_assert(check_route_pattern("/") == route_error::none);
static_assert(check_route_pattern("/users/:id/posts/:post_id") == route_error::none);
static_assert(check_route_pattern("/a b/c:d") == route_error::none); // ':' is only special at a segment's start.
static_assert(check_route_pattern("users") == route_error::no_leading_slash);
static_assert(check_route_pattern("") == route_error::no_leading_slash);
static_assert(check_route_pattern("/search?q") == route_error::query_or_fragment);
static_assert(check_route_pattern("/page#top") == route_error::query_or_fragment);
static_assert(check_route_pattern("/users/:") == route_error::empty_parameter_name);
static_assert(check_route_pattern("/users/:/posts") == route_error::empty_parameter_name);
static_assert(check_route_pattern("/users/:1d") == route_error::invalid_parameter_name);
static_assert(check_route_pattern("/users/:id([0-9]+)") == route_error::invalid_parameter_name);
static_assert(check_route_pattern("/a/:id/b/:id") == route_error::duplicate_parameter_name);
static_assert(check_route_pattern("/a/:id/b/:idx") == route_error::none);

// and a string literal converts (at compile time).
static_assert(route_pattern{"/users/:id"}.view() == "/users/:id");

TEST_CASE("route patterns made at runtime are checked", "[http][router][route_pattern]")
{
    struct test_case
    {
        std::string_view str;
        route_error      want;
    };

    auto tc = GENERATE(test_case{.str = "/users/:id", .want = route_error::none},
                       test_case{.str = "users", .want = route_error::no_leading_slash},
                       test_case{.str = "/users/:", .want = route_error::empty_parameter_name},
                       test_case{.str = "/:a/:a", .want = route_error::duplicate_parameter_name});
    CAPTURE(tc.str);

    // e.g. from configuration.
    const std::string str{tc.str};

    auto parsed = route_pattern::parse(str);
    if (tc.want == route_error::none)
    {
        REQUIRE(parsed.has_value());
        CHECK(parsed->view() == str);
    }
    else
    {
        REQUIRE_FALSE(parsed.has_value());
        CHECK(parsed.error() == tc.want);
        CHECK_FALSE(parsed.error().message().empty());
    }
}
