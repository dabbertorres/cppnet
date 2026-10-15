#include "coro/task.hpp"

#include <expected>
#include <system_error>

#include <catch.hpp>

#include <catch2/catch_test_macros.hpp>

#include "coro/sync_wait.hpp"

TEST_CASE("task of std::expected can be destroyed", "[coro][task]")
{
    // Comparing the task's handle to nullptr used to find std::expected's operator== through ADL, which some standard
    // libraries don't constrain, and so recursed forever.
    auto make = []() -> net::coro::task<std::expected<int*, std::error_condition>> { co_return nullptr; };

    auto value = test::sync_wait(make());
    REQUIRE(value.has_value());
    CHECK(*value == nullptr);
}

namespace
{

net::coro::task<int> one() { co_return 1; }

net::coro::task<long> count_to(long n)
{
    long total = 0;
    for (long i = 0; i < n; ++i) total += co_await one();
    co_return total;
}

}

TEST_CASE("co_awaiting tasks that finish without suspending doesn't grow the stack", "[coro][task]")
{
    // With symmetric transfer, each of these nests a little deeper, unless the compiler makes the transfers tail calls
    // (which GCC doesn't without optimizations, or with ASan), and the stack overflows.
    CHECK(test::sync_wait(count_to(1'000'000)) == 1'000'000);
}
