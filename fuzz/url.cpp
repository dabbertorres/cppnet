// fuzz_url checks that url's functions don't crash (they're noexcept, so exceptions count), and that:
// - build() output parses back to the same url.
// - build() and origin_form() output only printable ASCII, so nothing in a url can end an HTTP request line.
#include "url.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <print>
#include <string>
#include <string_view>

namespace
{

[[noreturn]] void fail(const char* what, std::string_view input, std::string_view got)
{
    std::println(stderr, "PROPERTY FAILED: {}\n  input: [{}]\n  got:   [{}]", what, input, got);
    std::abort();
}

}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    std::string_view input{reinterpret_cast<const char*>(data), size};

    (void)net::url::decode(input);
    (void)net::url::encode(input);

    auto parsed = net::url::parse(input);
    if (!parsed.has_value()) return 0;

    const auto& u     = parsed.to_value();
    auto        built = u.build();

    for (unsigned char c : built)
        if (c <= 0x20 || c >= 0x7F) fail("build() emitted a control, space, or non-ASCII byte", input, built);

    auto reparsed = net::url::parse(built);
    if (!reparsed.has_value()) fail("build() output doesn't parse", input, built);
    if (!(reparsed.to_value() == u)) fail("build() doesn't round-trip", input, built);

    auto origin = u.origin_form();
    for (unsigned char c : origin)
        if (c <= 0x20 || c >= 0x7F) fail("origin_form() emitted a control, space, or non-ASCII byte", input, origin);

    return 0;
}
