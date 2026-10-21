#include "http/structured_fields.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>

#include "http/sf_serializer.hpp"
#include "http/structured_field_tests.hpp"

namespace sf = net::http::sf;

using namespace std::string_view_literals;

namespace
{

// serialized parses value as kind, and serializes what it parses to (RFC 9651's canonical form), or nullopt if it
// doesn't parse.
std::optional<std::string> serialized(structured_field_tests::kind kind, std::string_view value)
{
    switch (kind)
    {
    case structured_field_tests::kind::item:
        if (auto i = test::parse_item(value)) return test::serialize(*i);
        return std::nullopt;
    case structured_field_tests::kind::list:
        if (auto l = test::parse_list(value)) return test::serialize(*l);
        return std::nullopt;
    case structured_field_tests::kind::dictionary:
        if (auto d = test::parse_dictionary(value)) return test::serialize(*d);
        return std::nullopt;
    }
    return std::nullopt;
}

// within is whether text is a view of value's characters (or empty, as a number's is).
bool within(std::string_view text, std::string_view value)
{
    if (text.empty()) return true;
    return std::less_equal<>{}(value.data(), text.data())
        && std::less_equal<>{}(text.data() + text.size(), value.data() + value.size());
}

}

TEST_CASE("the httpwg's Structured Fields tests parse as they say", "[http][structured-fields][httpwg]")
{
    const auto& tc = GENERATE(from_range(structured_field_tests::cases));
    INFO(tc.name);

    std::vector<std::string> lines;
    lines.reserve(tc.line_count);
    for (std::size_t i = 0; i < tc.line_count; ++i) lines.emplace_back(tc.lines.at(i));
    std::string            buffer;
    const std::string_view value = sf::join_lines(lines, buffer);
    CAPTURE(value);

    const auto got = serialized(tc.kind, value);
    switch (tc.outcome)
    {
    case structured_field_tests::outcome::must_fail: CHECK_FALSE(got); break;
    case structured_field_tests::outcome::can_fail:
    case structured_field_tests::outcome::pass:
    {
        std::string canonical{tc.canonical.value_or(value)};
        if (tc.outcome == structured_field_tests::outcome::pass) REQUIRE(got);
        if (got) CHECK(*got == canonical);
        break;
    }
    }
}

TEST_CASE("large values parse, as the httpwg's large tests make them", "[http][structured-fields][httpwg]")
{
    // large-generated.json's cases, made here rather than kept in structured_field_tests.hpp: each value is its own
    // canonical form.
    const auto joined = [](std::size_t count, auto&& member)
    {
        std::string out;
        for (std::size_t i = 0; i < count; ++i)
        {
            if (i > 0) out += ", ";
            out += member(i);
        }
        return out;
    };

    std::string many_params = "foo";
    for (std::size_t i = 0; i < 256; ++i) many_params += ";a" + std::to_string(i) + "=1";
    std::string inner = "(";
    for (std::size_t i = 0; i < 256; ++i) inner += (i > 0 ? " " : "") + std::to_string(i);
    inner += ')';
    std::string escaped = "\"";
    for (std::size_t i = 0; i < 1'024; ++i) escaped += "\\\"";
    escaped += '"';
    std::string bytes = ":";
    for (std::size_t i = 0; i < 16'384 / 3; ++i) bytes += "YWFh"; // "aaa"
    bytes += "YQ==:";                                             // and the last "a"

    struct test_case
    {
        std::string_view             what;
        structured_field_tests::kind kind = structured_field_tests::kind::item;
        std::string                  value;
    };
    const auto tc = GENERATE_REF(
        test_case{.what  = "large dictionary",
                  .kind  = structured_field_tests::kind::dictionary,
                  .value = joined(1'024, [](std::size_t i) { return "a" + std::to_string(i) + "=1"; })},
        test_case{.what  = "large dictionary key",
                  .kind  = structured_field_tests::kind::dictionary,
                  .value = std::string(64, 'a') + "=1"},
        test_case{.what  = "large list",
                  .kind  = structured_field_tests::kind::list,
                  .value = joined(1'024, [](std::size_t i) { return "a" + std::to_string(i); })},
        test_case{.what  = "large parameterised list",
                  .kind  = structured_field_tests::kind::list,
                  .value = joined(1'024, [](std::size_t i) { return "foo;a" + std::to_string(i) + "=1"; })},
        test_case{.what = "large params", .kind = structured_field_tests::kind::list, .value = many_params},
        test_case{.what  = "large param key",
                  .kind  = structured_field_tests::kind::list,
                  .value = "foo;" + std::string(64, 'a') + "=1"},
        test_case{.what = "large string", .value = "\"" + std::string(1'024, '=') + "\""},
        test_case{.what = "large escaped string", .value = escaped},
        test_case{.what = "large token", .value = std::string(512, 'a')},
        test_case{.what = "large byte sequence", .value = bytes},
        test_case{.what = "large inner list", .kind = structured_field_tests::kind::list, .value = inner});
    INFO(tc.what);

    const auto got = serialized(tc.kind, tc.value);
    REQUIRE(got);
    CHECK(*got == tc.value);
}

TEST_CASE("a dictionary's members are reported in order, as views of the value",
          "[http][structured-fields][dictionary]")
{
    const std::string value =
        R"(a=(1 "two";x 3.5), b="q\"uo\\te", c=:aGk=:;p=?0, d=%"f%c3%bcr", e=@-5, f=?0, g=-4.25;p=1;p=*t, h;q, i=tok/en:1)";

    std::vector<std::string> keys;
    const auto               parsed = sf::parse_dictionary(value,
                                                           [&](std::string_view key, const sf::member& m)
                                                           {
                                                 CHECK(within(key, value));
                                                 keys.emplace_back(key);
                                                 if (key == "a")
                                                 {
                                                     const auto* list = std::get_if<sf::inner_list>(&m);
                                                     REQUIRE(list);
                                                     CHECK(list->params().empty());
                                                     std::vector<sf::item> items;
                                                     for (const auto& i : *list) items.push_back(i);
                                                     REQUIRE(items.size() == 3);
                                                     CHECK(items[0].value.integer() == 1);
                                                     CHECK(items[0].params.empty());
                                                     CHECK(items[1].value.type == sf::item_type::string);
                                                     CHECK(items[1].value.text == "two");
                                                     CHECK(within(items[1].value.text, value));
                                                     CHECK(items[1].params.find("x").value().boolean() == true);
                                                     CHECK(items[2].value.decimal() == 3.5);
                                                     CHECK(items[2].value.number == 3'500);
                                                     return;
                                                 }

                                                 const auto* i = std::get_if<sf::item>(&m);
                                                 REQUIRE(i);
                                                 std::string decoded;
                                                 sf::decode(i->value, decoded);
                                                 CHECK(within(i->value.text, value));
                                                 if (key == "b")
                                                 {
                                                     CHECK(i->value.type == sf::item_type::string);
                                                     CHECK(i->value.text == R"(q\"uo\\te)"); // as written
                                                     CHECK(decoded == R"(q"uo\te)");
                                                 }
                                                 else if (key == "c")
                                                 {
                                                     CHECK(i->value.type == sf::item_type::byte_sequence);
                                                     CHECK(i->value.text == "aGk=");
                                                     CHECK(decoded == "hi");
                                                     CHECK(i->params.find("p").value().boolean() == false);
                                                 }
                                                 else if (key == "d")
                                                 {
                                                     CHECK(i->value.type == sf::item_type::display_string);
                                                     CHECK(i->value.text == "f%c3%bcr");
                                                     CHECK(decoded == "f\xc3\xbcr");
                                                 }
                                                 else if (key == "e")
                                                 {
                                                     CHECK(i->value.date() == -5);
                                                     CHECK_FALSE(i->value.integer());
                                                 }
                                                 else if (key == "f")
                                                 {
                                                     CHECK(i->value.boolean() == false);
                                                 }
                                                 else if (key == "g")
                                                 {
                                                     CHECK(i->value.number == -4'250);
                                                     CHECK(i->value.decimal() == -4.25);
                                                     // Both parameters are there, in order; find() gives the last.
                                                     std::vector<std::string_view> names;
                                                     for (const auto& p : i->params) names.push_back(p.key);
                                                     CHECK(names == std::vector{"p"sv, "p"sv});
                                                     const auto p = i->params.find("p");
                                                     REQUIRE(p);
                                                     CHECK(p->type == sf::item_type::token);
                                                     CHECK(p->text == "*t");
                                                     CHECK(i->params.text() == ";p=1;p=*t");
                                                     CHECK_FALSE(i->params.find("q"));
                                                 }
                                                 else if (key == "h")
                                                 {
                                                     // No value: Boolean true, with its parameters.
                                                     CHECK(i->value.boolean() == true);
                                                     CHECK(i->params.find("q").value().boolean() == true);
                                                 }
                                                 else if (key == "i")
                                                 {
                                                     CHECK(i->value.type == sf::item_type::token);
                                                     CHECK(decoded == "tok/en:1");
                                                     CHECK(i->value.text == "tok/en:1");
                                                 }
                                                           });
    CHECK(parsed);
    CHECK(keys == std::vector<std::string>{"a", "b", "c", "d", "e", "f", "g", "h", "i"});
}

TEST_CASE("a dictionary's repeated keys are each reported", "[http][structured-fields][dictionary]")
{
    std::vector<std::pair<std::string, std::int64_t>> members;
    const auto                                        parsed =
        sf::parse_dictionary("a=1, b=2, a=3",
                             [&](std::string_view key, const sf::member& m)
                             { members.emplace_back(key, std::get<sf::item>(m).value.integer().value()); });
    CHECK(parsed);
    CHECK(members
          == std::vector<std::pair<std::string, std::int64_t>>{
              {"a", 1},
              {"b", 2},
              {"a", 3}
    });

    // RFC 9651's ordered map: the later replaces the earlier, in its place.
    CHECK(test::serialize(test::parse_dictionary("a=1, b=2, a=3").value()) == "a=3, b=2");
}

TEST_CASE("a value that fails, fails whole, after the members before what's malformed",
          "[http][structured-fields][malformed]")
{
    std::vector<std::string> seen;
    const auto               dict =
        sf::parse_dictionary("a=1, b=2, C=3", [&](std::string_view key, const sf::member&) { seen.emplace_back(key); });
    REQUIRE_FALSE(dict);
    CHECK(dict.error() == std::errc::invalid_argument);
    CHECK(seen == std::vector<std::string>{"a", "b"});

    std::size_t count = 0;
    const auto  list  = sf::parse_list("1, 2, 3,", [&](const sf::member&) { ++count; });
    REQUIRE_FALSE(list);
    CHECK(list.error() == std::errc::invalid_argument);
    CHECK(count == 3);

    const auto item = sf::parse_item("1 2");
    REQUIRE_FALSE(item);
    CHECK(item.error() == std::errc::invalid_argument);
}

TEST_CASE("an empty list or dictionary has no members, and an empty inner list no items",
          "[http][structured-fields][empty]")
{
    const auto  value = GENERATE(""sv, "   "sv);
    std::size_t count = 0;
    CHECK(sf::parse_list(value, [&](const sf::member&) { ++count; }));
    CHECK(sf::parse_dictionary(value, [&](std::string_view, const sf::member&) { ++count; }));
    CHECK(count == 0);
    CHECK_FALSE(sf::parse_item(value));

    bool reported = false;
    CHECK(sf::parse_list("(  );a",
                         [&](const sf::member& m)
                         {
                             const auto& list = std::get<sf::inner_list>(m);
                             CHECK(list.empty());
                             CHECK(list.begin() == list.end());
                             CHECK(list.params().find("a"));
                             reported = true;
                         }));
    CHECK(reported);
}

TEST_CASE("numbers are read exactly, to their limits", "[http][structured-fields][number]")
{
    struct test_case
    {
        std::string_view value;
        sf::item_type    type   = sf::item_type::integer;
        std::int64_t     number = 0;
    };
    const auto tc = GENERATE(
        test_case{.value = "999999999999999", .number = 999'999'999'999'999},
        test_case{.value = "-999999999999999", .number = -999'999'999'999'999},
        test_case{.value = "000000000000001", .number = 1},
        test_case{.value = "-0", .number = 0},
        test_case{.value = "999999999999.999", .type = sf::item_type::decimal, .number = 999'999'999'999'999},
        test_case{.value = "-999999999999.999", .type = sf::item_type::decimal, .number = -999'999'999'999'999},
        test_case{.value = "0.1", .type = sf::item_type::decimal, .number = 100},
        test_case{.value = "-0.01", .type = sf::item_type::decimal, .number = -10},
        test_case{.value = "1.000", .type = sf::item_type::decimal, .number = 1'000},
        test_case{.value = "@-999999999999999", .type = sf::item_type::date, .number = -999'999'999'999'999});
    CAPTURE(tc.value);

    const auto parsed = sf::parse_item(tc.value);
    REQUIRE(parsed);
    CHECK(parsed->value.type == tc.type);
    CHECK(parsed->value.number == tc.number);
}

TEST_CASE("join_lines joins a field's lines with commas, copying only if there are several",
          "[http][structured-fields][join]")
{
    std::string buffer = "left over";

    CHECK(sf::join_lines({}, buffer).empty());

    const std::vector<std::string> one{"u=1"};
    const auto                     joined_one = sf::join_lines(one, buffer);
    CHECK(joined_one == "u=1");
    CHECK(joined_one.data() == one.front().data());

    const std::vector<std::string> several{"u=1", "", "i"};
    CHECK(sf::join_lines(several, buffer) == "u=1, , i");
    CHECK(buffer == "u=1, , i");
    CHECK_FALSE(sf::parse_dictionary(sf::join_lines(several, buffer), [](std::string_view, const sf::member&) {}));

    const std::vector<std::string> two{"a=\"x", "y\""}; // a String split over two lines
    const auto                     joined_two = sf::join_lines(two, buffer);
    const auto                     dict       = test::parse_dictionary(joined_two);
    REQUIRE(dict.has_value());
    CHECK(test::serialize(*dict) == R"(a="x, y")");
}

TEST_CASE("display strings must be UTF-8, and byte sequences a length bytes can have",
          "[http][structured-fields][item]")
{
    struct test_case
    {
        std::string_view value;
        bool             valid   = false;
        std::string_view decoded = {};
    };
    const auto tc =
        GENERATE(test_case{.value = R"(%"%f0%9f%98%80")", .valid = true, .decoded = "\xf0\x9f\x98\x80"},
                 test_case{.value = R"(%"%ed%9f%bf")", .valid = true, .decoded = "\xed\x9f\xbf"},        // U+D7FF
                 test_case{.value = R"(%"%f4%8f%bf%bf")", .valid = true, .decoded = "\xf4\x8f\xbf\xbf"}, // U+10FFFF
                 test_case{.value = R"(%"%c3a")"},            // a character cut short by an ASCII one
                 test_case{.value = R"(%"%c3")"},             // cut short by the end
                 test_case{.value = R"(%"%ed%a0%80")"},       // a surrogate
                 test_case{.value = R"(%"%f4%90%80%80")"},    // past U+10FFFF
                 test_case{.value = R"(%"%c0%80")"},          // overlong
                 test_case{.value = R"(%"%e0%80%80")"},       // overlong
                 test_case{.value = R"(%"%80")"},             // a continuation byte alone
                 test_case{.value = R"(%"%f8%88%80%80%80")"}, // a 5-byte form
                 test_case{.value = ":YQ:", .valid = true, .decoded = "a"},
                 test_case{.value = ":YWI:", .valid = true, .decoded = "ab"},
                 test_case{.value = ":YWJj====:", .valid = true, .decoded = "abc"},
                 test_case{.value = ":Y:"},     // 6 bits, no byte
                 test_case{.value = ":YWJjZ:"}, // the same, after 3 bytes
                 test_case{.value = ":Y=:"},
                 test_case{.value = ":YQ=Q:"}); // padding before the end
    CAPTURE(tc.value);

    const auto parsed = sf::parse_item(tc.value);
    REQUIRE(parsed.has_value() == tc.valid);
    if (!parsed) return;
    std::string decoded;
    sf::decode(parsed->value, decoded);
    CHECK(decoded == tc.decoded);
}
