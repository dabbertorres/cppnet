// fuzz_base64 checks that base64's functions don't crash, and that, with and without padding:
// - decoding what encode made gives back the input.
// - anything decode accepts is what encode makes of what it decoded to: there's only one encoding of any bytes.
#include "encoding/base64.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <print>
#include <span>
#include <string_view>

namespace
{

namespace base64 = net::encoding::base64;

[[noreturn]] void fail(const char* what, std::string_view input, std::string_view got)
{
    std::println(stderr, "PROPERTY FAILED: {}\n  input: [{}]\n  got:   [{}]", what, input, got);
    std::abort();
}

void check(const base64::encoding& enc, std::string_view input)
{
    const auto bytes = std::as_bytes(std::span{input});

    const auto encoded = enc.encode(bytes);
    const auto decoded = enc.decode(encoded);
    if (!decoded.has_value()) fail("encode() output doesn't decode", input, encoded);
    if (!std::ranges::equal(*decoded, bytes)) fail("encode() doesn't round-trip", input, encoded);

    const auto accepted = enc.decode(input);
    if (!accepted.has_value()) return;

    const auto reencoded = enc.encode(*accepted);
    if (reencoded != input) fail("decode() accepted a non-canonical encoding", input, reencoded);
}

}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    const std::string_view input{reinterpret_cast<const char*>(data), size};

    check(base64::encoding{}, input);
    check(base64::encoding{base64::url_alphabet(), 0}, input);
    return 0;
}
