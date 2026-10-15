#include "util/string_map.hpp"

#include <exception> // IWYU pragma: keep
#include <string_view>

#include <catch2/catch_test_macros.hpp>

using namespace std::string_view_literals;

TEST_CASE("erase by a string_view", "[util][string_map]")
{
    net::util::string_map<int> map{
        {"a", 1},
        {"b", 2}
    };

    CHECK(map.erase("a"sv) == 1);
    CHECK(map.erase("a"sv) == 0);
    CHECK(map.erase("missing"sv) == 0);

    CHECK_FALSE(map.contains("a"sv));
    CHECK(map.contains("b"sv));
}

TEST_CASE("extract by a string_view", "[util][string_map]")
{
    net::util::string_map<int> map{
        {"a", 1}
    };

    auto node = map.extract("a"sv);
    REQUIRE_FALSE(node.empty());
    CHECK(node.key() == "a");
    CHECK(node.mapped() == 1);
    CHECK(map.empty());

    CHECK(map.extract("a"sv).empty());
}
