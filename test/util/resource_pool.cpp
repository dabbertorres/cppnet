#include "util/resource_pool.hpp"

#include <chrono>
#include <exception> // IWYU pragma: keep
#include <thread>
#include <utility>

#include <catch.hpp>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

using net::util::resource_pool;

TEST_CASE("get and put", "[util][resource_pool]")
{
    resource_pool<int> pool{4, 4};
    {
        auto res = pool.get();
        CHECK(pool.available_resources() == 0);
    }

    CHECK(pool.available_resources() > 0);
}

TEST_CASE("get multiple", "[util][resource_pool]")
{
    resource_pool<int> pool{4, 4};

    {
        auto r0 = pool.get();
        auto r1 = pool.get();
        auto r2 = pool.get();
        auto r3 = pool.get();

        CHECK_FALSE(pool.try_get().has_value());
    }

    CHECK(pool.available_resources() == 4);
}

TEST_CASE("does not go over max", "[util][resource_pool]")
{
    resource_pool<int> pool{1, 1};

    auto        res0 = pool.get();
    std::thread waiting([res = std::move(res0)]() { std::this_thread::sleep_for(std::chrono::milliseconds(10)); });

    CHECK_FALSE(pool.try_get().has_value());

    if (waiting.joinable()) waiting.join();

    CHECK(pool.available_resources() == 1);
}
