#include "url.hpp"

#include <catch2/catch.hpp>

TEST_CASE("url::parse '/'", "[url][urlparse]")
{
    auto result = net::url::parse("/");

    if (!result.has_value())
    {
        FAIL("expected value, but had an error: "
             << net::parse_state_string(result.to_error().failed_at));
    }

    auto u = result.to_value();
    REQUIRE(u.scheme.empty());
    REQUIRE(u.userinfo.username.empty());
    REQUIRE(u.userinfo.password.empty());
    REQUIRE(u.host.empty());
    REQUIRE(u.port.empty());
    REQUIRE(u.path == "/");
    REQUIRE(u.query.empty());
    REQUIRE(u.fragment.empty());
}

TEST_CASE("url::parse 'foo/'", "[url][urlparse]")
{
    auto result = net::url::parse("foo/");

    if (!result.has_value())
    {
        FAIL("expected value, but had an error: "
             << net::parse_state_string(result.to_error().failed_at));
    }

    auto u = result.to_value();
    REQUIRE(u.scheme.empty());
    REQUIRE(u.userinfo.username.empty());
    REQUIRE(u.userinfo.password.empty());
    REQUIRE(u.host == "foo");
    REQUIRE(u.port.empty());
    REQUIRE(u.path == "/");
    REQUIRE(u.query.empty());
    REQUIRE(u.fragment.empty());
}

TEST_CASE("url::parse 'foo:9123/'", "[url][urlparse]")
{
    auto result = net::url::parse("foo:9123/");

    if (!result.has_value())
    {
        FAIL("expected value, but had an error: "
             << net::parse_state_string(result.to_error().failed_at));
    }

    auto u = result.to_value();
    REQUIRE(u.scheme == "foo");
    REQUIRE(u.userinfo.username.empty());
    REQUIRE(u.userinfo.password.empty());
    REQUIRE(u.host.empty());
    REQUIRE(u.port.empty());
    REQUIRE(u.path == "9123/");
    REQUIRE(u.query.empty());
    REQUIRE(u.fragment.empty());
}

TEST_CASE("url::parse 'http:///'", "[url][urlparse]")
{
    auto result = net::url::parse("http:///");

    if (!result.has_value())
    {
        FAIL("expected value, but had an error: "
             << net::parse_state_string(result.to_error().failed_at));
    }

    auto u = result.to_value();
    REQUIRE(u.scheme == "http");
    REQUIRE(u.userinfo.username.empty());
    REQUIRE(u.userinfo.password.empty());
    REQUIRE(u.host.empty());
    REQUIRE(u.port.empty());
    REQUIRE(u.path == "/");
    REQUIRE(u.query.empty());
    REQUIRE(u.fragment.empty());
}

TEST_CASE("url::parse 'http://foo/'", "[url][urlparse]")
{
    auto result = net::url::parse("http://foo/");

    if (!result.has_value())
    {
        FAIL("expected value, but had an error: "
             << net::parse_state_string(result.to_error().failed_at));
    }

    auto u = result.to_value();
    REQUIRE(u.scheme == "http");
    REQUIRE(u.userinfo.username.empty());
    REQUIRE(u.userinfo.password.empty());
    REQUIRE(u.host == "foo");
    REQUIRE(u.port.empty());
    REQUIRE(u.path == "/");
    REQUIRE(u.query.empty());
    REQUIRE(u.fragment.empty());
}

TEST_CASE("url::parse 'http://foo:9123/'", "[url][urlparse]")
{
    auto result = net::url::parse("http://foo:9123/");

    if (!result.has_value())
    {
        FAIL("expected value, but had an error: "
             << net::parse_state_string(result.to_error().failed_at));
    }

    auto u = result.to_value();
    REQUIRE(u.scheme == "http");
    REQUIRE(u.userinfo.username.empty());
    REQUIRE(u.userinfo.password.empty());
    REQUIRE(u.host == "foo");
    REQUIRE(u.port == "9123");
    REQUIRE(u.path == "/");
    REQUIRE(u.query.empty());
    REQUIRE(u.fragment.empty());
}

TEST_CASE("url::parse 'http://user:password@host:9123/path?foo=bar#fragment'", "[url][urlparse]")
{
    auto result = net::url::parse("http://user:password@host:9123/path?foo=bar#fragment");

    if (!result.has_value())
    {
        FAIL("expected value, but had an error: "
             << net::parse_state_string(result.to_error().failed_at));
    }

    auto u = result.to_value();
    REQUIRE(u.scheme == "http");
    REQUIRE(u.userinfo.username == "user");
    REQUIRE(u.userinfo.password == "password");
    REQUIRE(u.host == "host");
    REQUIRE(u.port == "9123");
    REQUIRE(u.path == "/path");
    REQUIRE(u.query == net::url::query_values{{"foo", {"bar"}}});
    REQUIRE(u.fragment == "fragment");
}

TEST_CASE("url::parse 'http://user:password@host:9123/path?foo=bar'", "[url][urlparse]")
{
    auto result = net::url::parse("http://user:password@host:9123/path?foo=bar");

    if (!result.has_value())
    {
        FAIL("expected value, but had an error: "
             << net::parse_state_string(result.to_error().failed_at));
    }

    auto u = result.to_value();
    REQUIRE(u.scheme == "http");
    REQUIRE(u.userinfo.username == "user");
    REQUIRE(u.userinfo.password == "password");
    REQUIRE(u.host == "host");
    REQUIRE(u.port == "9123");
    REQUIRE(u.path == "/path");
    REQUIRE(u.query == net::url::query_values{{"foo", {"bar"}}});
    REQUIRE(u.fragment.empty());
}

TEST_CASE("url::parse 'http://user:password@host:9123/path#fragment'", "[url][urlparse]")
{
    auto result = net::url::parse("http://user:password@host:9123/path#fragment");

    if (!result.has_value())
    {
        FAIL("expected value, but had an error: "
             << net::parse_state_string(result.to_error().failed_at));
    }

    auto u = result.to_value();
    REQUIRE(u.scheme == "http");
    REQUIRE(u.userinfo.username == "user");
    REQUIRE(u.userinfo.password == "password");
    REQUIRE(u.host == "host");
    REQUIRE(u.port == "9123");
    REQUIRE(u.path == "/path");
    REQUIRE(u.query.empty());
    REQUIRE(u.fragment == "fragment");
}

TEST_CASE("url::parse '/path?foo=bar&baz=xyzzy&qux=plugh'", "[url][urlparse]")
{
    auto result = net::url::parse("/path?foo=bar&baz=xyzzy&qux=plugh");

    if (!result.has_value())
    {
        FAIL("expected value, but had an error: "
             << net::parse_state_string(result.to_error().failed_at));
    }

    auto u = result.to_value();
    REQUIRE(u.scheme.empty());
    REQUIRE(u.userinfo.username.empty());
    REQUIRE(u.userinfo.password.empty());
    REQUIRE(u.host.empty());
    REQUIRE(u.port.empty());
    REQUIRE(u.path == "/path");
    REQUIRE(u.query
            == net::url::query_values{
                {"foo", {"bar"}},
                {"baz", {"xyzzy"}},
                {"qux", {"plugh"}},
            });
    REQUIRE(u.fragment.empty());
}

TEST_CASE("url::parse '/path?foo=bar&foo=baz&foo=qux'", "[url][urlparse]")
{
    auto result = net::url::parse("/path?foo=bar&foo=baz&xyzzy=plugh&foo=qux");

    if (!result.has_value())
    {
        FAIL("expected value, but had an error: "
             << net::parse_state_string(result.to_error().failed_at));
    }

    auto u = result.to_value();
    REQUIRE(u.scheme.empty());
    REQUIRE(u.userinfo.username.empty());
    REQUIRE(u.userinfo.password.empty());
    REQUIRE(u.host.empty());
    REQUIRE(u.port.empty());
    REQUIRE(u.path == "/path");
    REQUIRE(u.query
            == net::url::query_values{
                {"foo", {"bar", "baz", "qux"}},
                {"xyzzy", {"plugh"}},
            });
    REQUIRE(u.fragment.empty());
}

TEST_CASE("url encoding 'foo%bar' <=> 'foo%25bar'", "[url][urlencoding]")
{
    constexpr auto input  = "foo%bar";
    constexpr auto expect = "foo%25bar";

    SECTION("encode")
    {
        auto result = net::url::encode(input);
        REQUIRE(result == expect);
    }

    SECTION("decode")
    {
        auto result = net::url::decode(expect);
        REQUIRE(result == input);
    }
}
