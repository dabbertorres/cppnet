#include "coro/generator.hpp"

#include <stdexcept>
#include <utility>
#include <vector>

#include <catch.hpp>

#include <catch2/catch_test_macros.hpp>

namespace
{

net::coro::generator<int> count_to(int n)
{
    for (auto i = 1; i <= n; ++i) co_yield i;
}

net::coro::generator<int> yield_then_throw()
{
    co_yield 1;
    throw std::runtime_error{"boom"};
}

}

TEST_CASE("range-for yields all values", "[coro][generator]")
{
    std::vector<int> values;
    for (auto v : count_to(3)) values.push_back(v);

    CHECK(values == std::vector{1, 2, 3});
}

TEST_CASE("range-for propagates exceptions", "[coro][generator]")
{
    std::vector<int> values;

    auto iterate = [&]
    {
        for (auto v : yield_then_throw()) values.push_back(v);
    };

    CHECK_THROWS_AS(iterate(), std::runtime_error);
    CHECK(values == std::vector{1});
}

TEST_CASE("call operator propagates exceptions", "[coro][generator]")
{
    auto gen = yield_then_throw();

    REQUIRE(gen);
    CHECK(gen() == 1);
    CHECK_THROWS_AS(static_cast<bool>(gen), std::runtime_error);
}

TEST_CASE("move constructed generator", "[coro][generator]")
{
    auto original = count_to(2);
    auto moved    = std::move(original);

    std::vector<int> values;
    for (auto v : moved) values.push_back(v);

    CHECK(values == std::vector{1, 2});
}
