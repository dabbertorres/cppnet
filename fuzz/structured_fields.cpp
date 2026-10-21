// fuzz_structured_fields checks that Structured Fields' parsing doesn't crash on any value, and that:
// - parsing it as an Item, a List and a Dictionary gives the same each time (the members' views read again the same).
// - what parses, serialized (RFC 9651's canonical form, test/http/sf_serializer.hpp), parses again to the same, and
//   serializes to the same again.
// - parse_priority() agrees with the Dictionary: over two bases, the u and i the Dictionary has (the last of each, as
//   RFC 9651's ordered map has them), when valid, are what it gives, and what's missing or invalid leaves each base's;
//   and a value that doesn't parse leaves both bases whole. So that a valid member followed by an invalid one is
//   common, it's checked for the value with valid ones before it ("u=1, i, " and the value) too.
// - a priority it gives, written by to_string(), parses back to itself.
//
// The input is the value (a field's lines joined).
#include "http/structured_fields.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "http/priority.hpp"
#include "http/sf_serializer.hpp"

namespace
{

namespace sf = net::http::sf;

using net::http::priority;

[[noreturn]] void fail(const char* what, std::string_view input)
{
    std::print(stderr, "PROPERTY FAILED: {}\n  input:", what);
    for (const char c : input) std::print(stderr, " {:02x}", static_cast<unsigned char>(c));
    std::println(stderr, "");
    std::abort();
}

// check_round_trip checks that parse(value) is deterministic, and that what it gives serializes to a value that parses
// to the same, and serializes the same.
template<typename Parse>
void check_round_trip(std::string_view input, Parse parse, const char* what)
{
    const auto first = parse(input);
    if (parse(input) != first) fail(what, input);
    if (!first) return;

    const std::string serialized = test::serialize(*first);
    const auto        again      = parse(serialized);
    if (!again) fail(what, input);
    if (*again != *first) fail(what, input);
    if (test::serialize(*again) != serialized) fail(what, input);
}

// expected_priority is what parse_priority() should make of a parsed Dictionary over base.
priority expected_priority(const test::sf_dictionary& dict, priority base)
{
    for (const auto& [key, m] : dict)
    {
        const auto* i = std::get_if<test::sf_item>(&m);
        if (i == nullptr) continue;
        if (key == "u" && i->value.type == sf::item_type::integer && i->value.number >= 0 && i->value.number <= 7)
        {
            base.urgency = static_cast<std::uint8_t>(i->value.number);
        }
        else if (key == "i" && i->value.type == sf::item_type::boolean)
        {
            base.incremental = i->value.number != 0;
        }
    }
    return base;
}

// check_priority checks parse_priority() on value, against the Dictionary it parses to, and to_string() on what it
// gives.
void check_priority(std::string_view value)
{
    const priority base_a{.urgency = 0, .incremental = false};
    const priority base_b{.urgency = 7, .incremental = true};
    const priority got_a = net::http::parse_priority(value, base_a);
    const priority got_b = net::http::parse_priority(value, base_b);
    if (const auto dict = test::parse_dictionary(value))
    {
        if (got_a != expected_priority(*dict, base_a)) fail("priority over base a", value);
        if (got_b != expected_priority(*dict, base_b)) fail("priority over base b", value);
    }
    else if (got_a != base_a || got_b != base_b)
    {
        fail("priority of a value that doesn't parse", value);
    }

    for (const priority p : {got_a, got_b})
    {
        if (net::http::parse_priority(net::http::to_string(p)) != p) fail("priority round trip", value);
    }
}

}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    const std::string_view input{reinterpret_cast<const char*>(data), size};

    check_round_trip(input, [](std::string_view v) { return test::parse_item(v); }, "item round trip");
    check_round_trip(input, [](std::string_view v) { return test::parse_list(v); }, "list round trip");
    check_round_trip(input, [](std::string_view v) { return test::parse_dictionary(v); }, "dictionary round trip");

    check_priority(input);
    check_priority("u=1, i, " + std::string{input});
    return 0;
}
