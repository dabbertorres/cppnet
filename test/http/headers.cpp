#include "http/headers.hpp"

#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

using namespace std::string_view_literals;

TEST_CASE("fields are set and read with any kind of string", "[http][headers]")
{
    net::http::headers fields;
    const std::string  key   = "X-Key";
    std::string        value = "moved";

    fields.set("Content-Type", "text/plain");
    fields.set("X-String", std::string{"string"});
    fields.set(key, std::move(value));
    fields.set("X-List", {"a", "b"});
    fields.add("X-List", "c");
    fields.add(key, std::string{"more"});

    CHECK(fields.get("Content-Type") == "text/plain"sv);
    CHECK(fields["X-String"] == "string"sv);
    CHECK(fields.get(key) == "moved"sv);
    CHECK(fields.get_list("X-List") == std::vector{"a"sv, "b"sv, "c"sv});
    CHECK(std::ranges::distance(*fields.get_all(key)) == 2);
}

TEST_CASE("list-based fields are split on commas outside quotes", "[http][headers][get_list]")
{
    net::http::headers fields;
    fields.add("Accept"sv, "text/html, application/json ,,"sv);
    fields.add("Accept"sv, R"(text/plain; note="a, \"b\", c", */*)"sv);

    CHECK(fields.get_list("Accept"sv)
          == std::vector{"text/html"sv, "application/json"sv, R"(text/plain; note="a, \"b\", c")"sv, "*/*"sv});

    CHECK(fields.get_list("Missing"sv).empty());
}

TEST_CASE("Content-Length must be digits, and agree with itself", "[http][headers][content_length]")
{
    const auto length = [](std::initializer_list<std::string_view> values)
    {
        net::http::headers fields;
        for (auto value : values) fields.add("Content-Length"sv, value);
        return fields.get_content_length();
    };

    CHECK(length({"5"}) == 5);
    CHECK(length({"5, 5", "5"}) == 5);
    CHECK(length({}) == std::nullopt);
    CHECK(length({"5", "6"}) == std::nullopt);
    CHECK(length({"5x"}) == std::nullopt);
    CHECK(length({"-5"}) == std::nullopt);
    CHECK(length({""}) == std::nullopt);
    CHECK(length({"99999999999999999999999"}) == std::nullopt);
}

TEST_CASE("is_chunked only if chunked is last", "[http][headers][is_chunked]")
{
    const auto chunked = [](std::string_view value)
    {
        net::http::headers fields;
        fields.add("Transfer-Encoding"sv, value);
        return fields.is_chunked();
    };

    CHECK(chunked("chunked"));
    CHECK(chunked("gzip, Chunked"));
    CHECK_FALSE(chunked("chunked, gzip"));
    CHECK_FALSE(chunked("xchunked"));
}

TEST_CASE("remove a field", "[http][headers]")
{
    net::http::headers fields;
    fields.set("Content-Length"sv, "3"sv).set("X-Other"sv, "y"sv);

    fields.remove("content-length"sv);
    CHECK_FALSE(fields.get("Content-Length"sv).has_value());
    CHECK(fields.get("X-Other"sv) == "y");

    // removing a field that isn't there does nothing.
    fields.remove("Content-Length"sv);
}

TEST_CASE("fields keep their order, and names ignore case", "[http][headers]")
{
    net::http::headers fields;
    fields.set("Content-Type"sv, "text/plain"sv).add("set-cookie"sv, "a=1"sv).add("Set-Cookie"sv, "b=2"sv);
    fields.set("X-Last"sv, "1"sv);

    std::vector<std::string_view> names;
    for (const auto& [name, values] : fields) names.emplace_back(name);
    CHECK(names == std::vector{"Content-Type"sv, "set-cookie"sv, "X-Last"sv});

    CHECK(fields.get("content-type"sv) == "text/plain"sv);
    CHECK(fields.get_list("SET-COOKIE"sv) == std::vector{"a=1"sv, "b=2"sv});

    // set replaces every value.
    fields.set("SET-cookie"sv, "c=3"sv);
    CHECK(fields.get_list("Set-Cookie"sv) == std::vector{"c=3"sv});

    fields.remove("content-TYPE"sv);
    CHECK_FALSE(fields.get("Content-Type"sv).has_value());
    CHECK(fields.size() == 2);
}

TEST_CASE("headers are equal whatever order their names are in", "[http][headers]")
{
    const net::http::headers a{
        {"A",      {"1"}},
        {"B", {"2", "3"}},
    };
    const net::http::headers b{
        {"b", {"2", "3"}},
        {"a",      {"1"}},
    };
    const net::http::headers c{
        {"A",      {"1"}},
        {"B", {"3", "2"}},
    };

    CHECK(a == b);
    CHECK_FALSE(a == c); // a name's values are in order.
}

TEST_CASE("cleared and removed fields are gone, and their memory is reused", "[http][headers][clear]")
{
    const std::string long_value(100, 'a');

    net::http::headers fields;
    fields.set("Accept"sv, std::string_view{long_value}).add("X-Many"sv, "1"sv).add("X-Many"sv, "2"sv);

    fields.clear();
    CHECK(fields.empty());
    CHECK_FALSE(fields.get("Accept"sv).has_value());
    CHECK(fields.begin() == fields.end());

    // the fields added next take the cleared ones' places, with nothing left of their values.
    fields.add("Host"sv, "example.com"sv).set("X-Many"sv, "3"sv);
    CHECK(fields
          == net::http::headers{
              {  "Host", {"example.com"}},
              {"X-Many",           {"3"}}
    });

    const auto* reused = fields.get("Host"sv)->data();
    fields.remove("Host"sv);
    CHECK(fields
          == net::http::headers{
              {"X-Many", {"3"}}
    });

    // a value that fits in the removed field's memory is put there.
    fields.add("Accept"sv, "text/plain"sv);
    CHECK(fields.get("Accept"sv)->data() == reused);
    CHECK(fields
          == net::http::headers{
              {"X-Many",          {"3"}},
              {"Accept", {"text/plain"}}
    });
}

TEST_CASE("moved-from headers are empty", "[http][headers]")
{
    net::http::headers fields{
        {"Host", {"a"}}
    };
    net::http::headers moved{std::move(fields)};

    CHECK(moved
          == net::http::headers{
              {"Host", {"a"}}
    });
    // NOLINTBEGIN(bugprone-use-after-move,clang-analyzer-cplusplus.Move): what's left is checked.
    CHECK(fields.empty());

    fields.add("X"sv, "y"sv);
    CHECK(fields
          == net::http::headers{
              {"X", {"y"}}
    });
    // NOLINTEND(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
}

TEST_CASE("has_token finds an element of a list-based field", "[http][headers][get_list]")
{
    net::http::headers fields;
    fields.add("Connection"sv, "Keep-Alive, \"close\""sv).add("Connection"sv, "upgrade"sv);

    CHECK(fields.has_token("connection"sv, "keep-alive"sv));
    CHECK(fields.has_token("Connection"sv, "UPGRADE"sv));

    // a quoted string is one element, not the token inside it.
    CHECK_FALSE(fields.has_token("Connection"sv, "close"sv));
    CHECK_FALSE(fields.has_token("Transfer-Encoding"sv, "chunked"sv));
}
