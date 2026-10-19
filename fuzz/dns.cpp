// fuzz_dns checks that decoding DNS messages doesn't crash or hang on any input (compression pointers that loop, or
// chain, or counts past the data), and that:
// - decoding is deterministic: the same input decodes the same twice.
// - a message it decodes encodes (everything decoded is valid to encode), and the encoding decodes to the same message
//   and encodes to the same bytes again.
// - a query for any name encode() accepts decodes to the same query, the name without a final dot.
//
// The input is a message to decode; and, as a query, a 16-bit ID, a byte whose high bit says whether the query has an
// OPT record and whose others are the type, and the rest the name.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "dns/error.hpp"
#include "dns/message.hpp"

namespace
{

namespace dns = net::dns;

[[noreturn]] void fail(const char* what, std::span<const std::byte> input)
{
    std::print(stderr, "PROPERTY FAILED: {}\n  input:", what);
    for (const auto b : input) std::print(stderr, " {:02x}", std::to_integer<unsigned>(b));
    std::println(stderr, "");
    std::abort();
}

void check_decode(std::span<const std::byte> input)
{
    const auto decoded = dns::decode(input);
    const auto again   = dns::decode(input);
    if (decoded != again) fail("decoding the same bytes twice differs", input);

    if (!decoded.has_value())
    {
        if (decoded.error() != dns::errc::bad_response) fail("decoding failed with another error", input);
        return;
    }

    std::vector<std::byte> encoded;
    if (dns::encode(*decoded, encoded)) fail("a decoded message doesn't encode", input);

    const auto round_trip = dns::decode(encoded);
    if (!round_trip.has_value()) fail("a decoded message's encoding doesn't decode", input);
    if (*round_trip != *decoded) fail("a decoded message's encoding decodes to another message", input);

    std::vector<std::byte> twice;
    if (dns::encode(*round_trip, twice) || twice != encoded) fail("encoding the message again differs", input);
}

void check_query(std::span<const std::byte> input)
{
    if (input.size() < 3) return;

    const auto id =
        static_cast<std::uint16_t>((std::to_integer<unsigned>(input[0]) << 8U) | std::to_integer<unsigned>(input[1]));
    const auto bits = std::to_integer<std::uint8_t>(input[2]);
    const auto rest = input.subspan(3);

    const std::string_view name{reinterpret_cast<const char*>(rest.data()), rest.size()};
    const auto             query = dns::make_query(id, name, static_cast<dns::type>(bits & 0x7fU), (bits & 0x80U) != 0);

    std::vector<std::byte> encoded{std::byte{0xa5}};
    if (const auto err = dns::encode(query, encoded))
    {
        if (err != std::errc::invalid_argument) fail("encoding a query failed with another error", input);
        if (encoded.size() != 1) fail("encoding a query failed, changing what was there", input);
        return;
    }

    auto expected = query;
    if (name.ends_with('.')) expected.questions.front().name.pop_back();

    const auto decoded = dns::decode(std::span{encoded}.subspan(1));
    if (!decoded.has_value()) fail("a query's encoding doesn't decode", input);
    if (*decoded != expected) fail("a query's encoding decodes to another message", input);
}

}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    const std::span input{reinterpret_cast<const std::byte*>(data), size};

    check_decode(input);
    check_query(input);
    return 0;
}
