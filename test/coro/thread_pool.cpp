#include "coro/thread_pool.hpp"

#include <catch.hpp>

#include <catch2/catch_test_macros.hpp>

using net::coro::thread_pool;

TEST_CASE("schedule simple lambda", "[coro][thread_pool]")
{
    /* thread_pool pool; */

    /* auto future = pool.schedule([] { return true; }); */
    /* REQUIRE(future.valid()); */

    /* auto result = future.get(); */
    /* REQUIRE(result); */
}

TEST_CASE("schedule capturing lambda", "[coro][thread_pool]")
{
    /* int x = 3; */

    /* thread_pool pool; */

    /* auto future = pool.schedule([&] { return x += 5; }); */
    /* REQUIRE(future.valid()); */

    /* auto result = future.get(); */
    /* REQUIRE(result == 8); */
}

TEST_CASE("schedule functor object", "[coro][thread_pool]")
{
    /* struct doubler */
    /* { */
    /*     int x; */

    /*     int operator()() const { return x * 2; } */
    /* }; */

    /* thread_pool pool; */

    /* doubler d{.x = 7}; */

    /* auto future = pool.schedule(d); */
    /* REQUIRE(future.valid()); */

    /* auto result = future.get(); */
    /* REQUIRE(result == 14); */
}

TEST_CASE("schedule std::function", "[coro][thread_pool]")
{
    /* thread_pool pool; */

    /* int                  start  = 9; */
    /* std::function<int()> triple = [&] */
    /* { */
    /*     start *= 3; */
    /*     return start; */
    /* }; */

    /* auto future = pool.schedule(triple); */
    /* REQUIRE(future.valid()); */

    /* auto result = future.get(); */
    /* REQUIRE(result == 27); */
}
