#include "net.hpp"

#include <exception> // IWYU pragma: keep

#include <catch2/catch_test_macros.hpp>

TEST_CASE("hostname", "[net]")
{
    auto name = net::hostname();
    REQUIRE(name.has_value());
    CHECK_FALSE(name->empty());
    CHECK_FALSE(name->contains('\0'));
}
