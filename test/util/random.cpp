#include "util/random.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <set>
#include <span>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

TEST_CASE("random_bytes fills the whole span, and nothing past it", "[util][random]")
{
    // smaller than the batch (256 bytes), its size, and larger
    const auto size = GENERATE(std::size_t{1},
                               std::size_t{7},
                               std::size_t{16},
                               std::size_t{255},
                               std::size_t{256},
                               std::size_t{257},
                               std::size_t{4096});
    CAPTURE(size);

    // a guard byte each side, which must stay zero; every byte between becomes non-zero in one of a few draws (each
    // stays zero with a chance of 2^-64 over eight)
    std::vector<std::byte> buffer(size + 2);
    std::vector<std::byte> seen(size);
    for (int draw = 0; draw < 8; ++draw)
    {
        std::ranges::fill(buffer, std::byte{0});
        net::util::random_bytes(std::span{buffer}.subspan(1, size));
        for (std::size_t i = 0; i < size; ++i) seen[i] |= buffer[i + 1];
        REQUIRE(buffer.front() == std::byte{0});
        REQUIRE(buffer.back() == std::byte{0});
    }

    CHECK(std::ranges::none_of(seen, [](std::byte b) { return b == std::byte{0}; }));
}

TEST_CASE("random_bytes gives different bytes each call", "[util][random]")
{
    SECTION("two calls")
    {
        std::array<std::byte, 16> a{};
        std::array<std::byte, 16> b{};
        net::util::random_bytes(a);
        net::util::random_bytes(b);
        CHECK(a != b);
    }

    SECTION("small draws through several batches")
    {
        // 100 draws of 12 bytes take 1200 bytes, refilling the batch (256 bytes) part way through some draws: none
        // repeats another.
        std::set<std::array<std::byte, 12>> draws;
        for (int i = 0; i < 100; ++i)
        {
            std::array<std::byte, 12> draw{};
            net::util::random_bytes(draw);
            draws.insert(draw);
        }
        CHECK(draws.size() == 100);
    }

    SECTION("an empty span")
    {
        net::util::random_bytes({});
        SUCCEED();
    }
}
