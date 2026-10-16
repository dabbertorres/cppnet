#include "instrument/system.hpp"

#include <array>
#include <chrono>
#include <compare>
#include <cstddef>
#include <exception> // IWYU pragma: keep
#include <latch>
#include <ratio>
#include <thread>

#include <sys/socket.h>
#include <unistd.h>

#include <catch2/catch_test_macros.hpp>

namespace system_stats = net::instrument::system;

using namespace std::chrono_literals;

// NOTE: these are only implemented for Linux and macOS: elsewhere, most fail with std::errc::function_not_supported.

TEST_CASE("process CPU time grows", "[instrument][system]")
{
    auto before = system_stats::process_cpu_seconds();
    REQUIRE(before.has_value());

    // spin for a while.
    auto              until = std::chrono::steady_clock::now() + 50ms;
    volatile unsigned spin  = 0;
    while (std::chrono::steady_clock::now() < until) spin = spin + 1;

    auto after = system_stats::process_cpu_seconds();
    REQUIRE(after.has_value());
    CHECK(*after > *before);

    // in fractions of a second, rather than whole ones.
    CHECK(*after - *before < 10s);
}

TEST_CASE("process open file descriptors", "[instrument][system]")
{
    auto before = system_stats::process_open_fds();
    REQUIRE(before.has_value());
    CHECK(*before >= 3); // at least stdin, stdout and stderr.

    std::array<int, 2> fds{};
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds.data()) == 0);

    auto opened = system_stats::process_open_fds();

    ::close(fds[0]);
    ::close(fds[1]);

    auto closed = system_stats::process_open_fds();

    REQUIRE(opened.has_value());
    REQUIRE(closed.has_value());
    CHECK(*opened == *before + 2);
    CHECK(*closed == *before);

    auto max = system_stats::process_max_fds();
    REQUIRE(max.has_value());
    CHECK(*max >= *opened);
}

TEST_CASE("process memory", "[instrument][system]")
{
    auto virtual_memory = system_stats::process_virtual_memory();
    auto resident       = system_stats::process_resident_memory();
    REQUIRE(virtual_memory.has_value());
    REQUIRE(resident.has_value());

    CHECK(*resident > 0);
    CHECK(*virtual_memory >= *resident);

    auto max = system_stats::process_virtual_memory_max();
    REQUIRE(max.has_value());
    CHECK(*max >= *virtual_memory);

    // NOTE: only that it's available: a sanitizer's allocator may not count what it allocates.
    auto heap = system_stats::process_heap();
    CHECK(heap.has_value());
}

TEST_CASE("process start time", "[instrument][system]")
{
    auto start = system_stats::process_start_time();
    REQUIRE(start.has_value());

    // the tests started a while ago, but not long ago.
    auto now = std::chrono::system_clock::now();
    CHECK(*start <= now);
    CHECK(*start > now - 24h);
}

TEST_CASE("process threads", "[instrument][system]")
{
    // NOTE: a thread is started (and finished) first, as a sanitizer's runtime may start one of its own along with the
    //       first (ThreadSanitizer does).
    {
        std::jthread warm_up{[] {}};
    }

    // started counts the threads before and while running one more.
    const auto started = []
    {
        auto before = system_stats::process_threads();
        REQUIRE(before.has_value());
        CHECK(*before >= 1);

        std::latch running{1};
        std::latch done{1};

        std::jthread thread{[&]
                            {
                                running.count_down();
                                done.wait();
                            }};
        running.wait();

        auto during = system_stats::process_threads();
        done.count_down();

        REQUIRE(during.has_value());
        return static_cast<std::ptrdiff_t>(*during) - static_cast<std::ptrdiff_t>(*before);
    };

    // NOTE: tried a few times, as threads that earlier tests started may still be finishing, meanwhile.
    std::ptrdiff_t counted = 0;
    for (int attempt = 0; attempt < 10 && counted != 1; ++attempt) counted = started();

    CHECK(counted == 1);
}

TEST_CASE("process usage", "[instrument][system]")
{
    auto usage = system_stats::process_usage();
    REQUIRE(usage.has_value());

    CHECK(usage->user_cpu_time + usage->system_cpu_time > 0s);

    // in bytes, on every platform: at least what's resident now, which is well over a kilobyte per page.
    auto resident = system_stats::process_resident_memory();
    REQUIRE(resident.has_value());
    CHECK(usage->max_resident_set_size >= *resident / 2);
    CHECK(usage->max_resident_set_size > std::size_t{1'024} * 1'024);
}
