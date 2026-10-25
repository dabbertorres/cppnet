// fuzz_utf8 checks util::utf8_validator against a decoder of code points (test/util/utf8_reference.hpp):
// - fed a byte at a time, it fails at the byte the reference says can't be where it is, or else ends complete as the
//   reference says.
// - fed at once, and in pieces of several sizes (so characters and the ASCII path's words are split at each place),
//   it agrees: the piece that fails is the one holding that byte.
#include "util/utf8.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <print>
#include <span>

#include "util/utf8_reference.hpp"

namespace
{

[[noreturn]] void fail(const char* what, std::span<const std::byte> input, std::size_t detail)
{
    std::print(stderr, "PROPERTY FAILED: {} ({})\n  input:", what, detail);
    for (auto b : input) std::print(stderr, " {:02x}", static_cast<unsigned>(b));
    std::println(stderr);
    std::abort();
}

// in_pieces feeds input size bytes at a time, returning the start of the piece that failed (input's size if none), and
// whether it ended complete.
test::utf8_result in_pieces(std::span<const std::byte> input, std::size_t size)
{
    net::util::utf8_validator utf8;
    for (std::size_t at = 0; at < input.size(); at += size)
    {
        if (!utf8.feed(input.subspan(at, std::min(size, input.size() - at)))) return {.bad = at};
    }
    return {.bad = input.size(), .complete = utf8.complete()};
}

}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    const auto input    = std::as_bytes(std::span{data, size});
    const auto expected = test::utf8_reference(input);

    net::util::utf8_validator one;
    for (std::size_t i = 0; i < input.size(); ++i)
    {
        if (!one.feed(input[i]))
        {
            if (i != expected.bad) fail("a byte at a time, it failed at another byte", input, i);
            break;
        }
        if (i + 1 == input.size() && expected.bad != input.size()) fail("a byte at a time, it didn't fail", input, i);
    }
    if (expected.bad == input.size() && one.complete() != expected.complete)
    {
        fail("a byte at a time, it ended otherwise", input, expected.complete ? 1 : 0);
    }

    for (const std::size_t piece : {std::size_t{1},
                                    std::size_t{3},
                                    std::size_t{7},
                                    std::size_t{8},
                                    std::size_t{9},
                                    std::size_t{16},
                                    std::max(input.size(), std::size_t{1})})
    {
        const auto got = in_pieces(input, piece);
        if (expected.bad == input.size())
        {
            if (got != expected) fail("in pieces, a valid input wasn't taken as such", input, piece);
        }
        else if (got.bad != expected.bad / piece * piece)
        {
            fail("in pieces, another piece failed", input, piece);
        }
    }
    return 0;
}
