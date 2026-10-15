#include "util/type_map.hpp"

#include <exception> // IWYU pragma: keep
#include <string>

#include <catch.hpp>

#include <catch2/catch_test_macros.hpp>

using net::util::type_map;

TEST_CASE("a type_map holds one value of each type", "[util][type_map]")
{
    type_map m;
    CHECK(m.empty());
    CHECK(m.get<int>() == nullptr);

    m.emplace<int>(1);
    m.emplace<std::string>(3, 'x');

    REQUIRE(m.get<int>() != nullptr);
    CHECK(*m.get<int>() == 1);
    REQUIRE(m.get<std::string>() != nullptr);
    CHECK(*m.get<std::string>() == "xxx");
    CHECK(m.get<long>() == nullptr); // only exactly the type
    CHECK(m.size() == 2);

    SECTION("emplace replaces a value of the same type")
    {
        CHECK(m.emplace<int>(2) == 2);
        CHECK(*m.get<int>() == 2);
        CHECK(m.size() == 2);
    }

    SECTION("values can be changed in place")
    {
        *m.get<int>() += 41;
        CHECK(*m.get<int>() == 42);
    }

    SECTION("erase removes only the value of its type")
    {
        CHECK(m.erase<int>());
        CHECK_FALSE(m.erase<int>());
        CHECK(m.get<int>() == nullptr);
        CHECK(*m.get<std::string>() == "xxx");
    }

    SECTION("copies are independent")
    {
        const type_map copy = m;
        *m.get<int>()       = 5;
        CHECK(*copy.get<int>() == 1);
    }
}
