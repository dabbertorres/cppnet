#include "util/resource_pool.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <thread>

#include <catch.hpp>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

using net::util::resource_pool;

TEST_CASE("get and put", "[util][resource_pool]")
{
    resource_pool<int, 4> pool;

    auto res = pool.get();
    pool.put(std::move(res));
}

TEST_CASE("get multiple", "[util][resource_pool]")
{
    resource_pool<int, 4> pool;

    auto _ = pool.get();
    _      = pool.get();
    _      = pool.get();
    _      = pool.get();

    CHECK_FALSE(pool.try_get().has_value());
}

TEST_CASE("does not go over max", "[util][resource_pool]")
{
    resource_pool<int, 1> pool;

    std::mutex              mu;
    std::condition_variable put_resource_back;

    auto res0 = pool.get();

    std::thread waiting(
        [&]
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            pool.put(std::move(res0));
            put_resource_back.notify_one();
        });

    CHECK_FALSE(pool.try_get().has_value());

    std::unique_lock lock(mu);

    auto got_resource =
        put_resource_back.wait_for(lock, std::chrono::milliseconds(100), [&] { return pool.try_get().has_value(); });
    CHECK(got_resource);

    if (waiting.joinable()) waiting.join();
}
