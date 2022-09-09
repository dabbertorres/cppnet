#include "url.hpp"

#include <catch.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("url::parse '/'", "[url][parse]")
{
    auto result = net::url::parse("/");

    if (!result.has_value())
        FAIL("expected value, but had an error: " << net::parse_state_string(result.to_error().failed_at));

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

TEST_CASE("url::parse 'foo/'", "[url][parse]")
{
    auto result = net::url::parse("foo/");

    if (!result.has_value())
        FAIL("expected value, but had an error: " << net::parse_state_string(result.to_error().failed_at));

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

TEST_CASE("url::parse 'foo:9123/'", "[url][parse]")
{
    auto result = net::url::parse("foo:9123/");

    if (!result.has_value())
        FAIL("expected value, but had an error: " << net::parse_state_string(result.to_error().failed_at));

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

TEST_CASE("url::parse 'http:///'", "[url][parse]")
{
    auto result = net::url::parse("http:///");

    if (!result.has_value())
        FAIL("expected value, but had an error: " << net::parse_state_string(result.to_error().failed_at));

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

TEST_CASE("url::parse 'http://foo/'", "[url][parse]")
{
    auto result = net::url::parse("http://foo/");

    if (!result.has_value())
        FAIL("expected value, but had an error: " << net::parse_state_string(result.to_error().failed_at));

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

TEST_CASE("url::parse 'http://foo:9123/'", "[url][parse]")
{
    auto result = net::url::parse("http://foo:9123/");

    if (!result.has_value())
        FAIL("expected value, but had an error: " << net::parse_state_string(result.to_error().failed_at));

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

TEST_CASE("url::parse 'http://user:password@host:9123/path?foo=bar#fragment'", "[url][parse]")
{
    auto result = net::url::parse("http://user:password@host:9123/path?foo=bar#fragment");

    if (!result.has_value())
        FAIL("expected value, but had an error: " << net::parse_state_string(result.to_error().failed_at));

    auto u = result.to_value();
    REQUIRE(u.scheme == "http");
    REQUIRE(u.userinfo.username == "user");
    REQUIRE(u.userinfo.password == "password");
    REQUIRE(u.host == "host");
    REQUIRE(u.port == "9123");
    REQUIRE(u.path == "/path");
    REQUIRE(u.query
            == net::url::query_values{
                {"foo", {"bar"}}
    });
    REQUIRE(u.fragment == "fragment");
}

TEST_CASE("url::parse 'http://user:password@host:9123/path?foo=bar'", "[url][parse]")
{
    auto result = net::url::parse("http://user:password@host:9123/path?foo=bar");

    if (!result.has_value())
        FAIL("expected value, but had an error: " << net::parse_state_string(result.to_error().failed_at));

    auto u = result.to_value();
    REQUIRE(u.scheme == "http");
    REQUIRE(u.userinfo.username == "user");
    REQUIRE(u.userinfo.password == "password");
    REQUIRE(u.host == "host");
    REQUIRE(u.port == "9123");
    REQUIRE(u.path == "/path");
    REQUIRE(u.query
            == net::url::query_values{
                {"foo", {"bar"}}
    });
    REQUIRE(u.fragment.empty());
}

TEST_CASE("url::parse 'http://user:password@host:9123/path#fragment'", "[url][parse]")
{
    auto result = net::url::parse("http://user:password@host:9123/path#fragment");

    if (!result.has_value())
        FAIL("expected value, but had an error: " << net::parse_state_string(result.to_error().failed_at));

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

TEST_CASE("url::parse '/path?foo=bar&baz=xyzzy&qux=plugh'", "[url][parse]")
{
    auto result = net::url::parse("/path?foo=bar&baz=xyzzy&qux=plugh");

    if (!result.has_value())
        FAIL("expected value, but had an error: " << net::parse_state_string(result.to_error().failed_at));

    auto u = result.to_value();
    REQUIRE(u.scheme.empty());
    REQUIRE(u.userinfo.username.empty());
    REQUIRE(u.userinfo.password.empty());
    REQUIRE(u.host.empty());
    REQUIRE(u.port.empty());
    REQUIRE(u.path == "/path");
    REQUIRE(u.query
            == net::url::query_values{
                {"foo", {"bar"}  },
                {"baz", {"xyzzy"}},
                {"qux", {"plugh"}},
    });
    REQUIRE(u.fragment.empty());
}

TEST_CASE("url::parse '/path?foo=bar&foo=baz&foo=qux'", "[url][parse]")
{
    auto result = net::url::parse("/path?foo=bar&foo=baz&xyzzy=plugh&foo=qux");

    if (!result.has_value())
        FAIL("expected value, but had an error: " << net::parse_state_string(result.to_error().failed_at));

    auto u = result.to_value();
    REQUIRE(u.scheme.empty());
    REQUIRE(u.userinfo.username.empty());
    REQUIRE(u.userinfo.password.empty());
    REQUIRE(u.host.empty());
    REQUIRE(u.port.empty());
    REQUIRE(u.path == "/path");
    REQUIRE(u.query
            == net::url::query_values{
                {"foo",   {"bar", "baz", "qux"}},
                {"xyzzy", {"plugh"}            },
    });
    REQUIRE(u.fragment.empty());
}

// crazy weird url: https://daniel.haxx.se/blog/2022/09/08/http-http-http-http-http-http-http/
TEST_CASE("url::parse 'http://http://http://@http://http://?http://#http://'", "[url][parse]")
{
    auto result = net::url::parse("http://http://http://@http://http://?http://#http://");

    if (!result.has_value())
        FAIL("expected value, but had an error: " << net::parse_state_string(result.to_error().failed_at));

    auto u = result.to_value();

    REQUIRE(u.scheme == "http");
    REQUIRE(u.userinfo.username == "http");
    REQUIRE(u.userinfo.password == "//http://");
    REQUIRE(u.host == "http");
    REQUIRE(u.port.empty());
    REQUIRE(u.path == "//http://");
    REQUIRE(u.query
            == net::url::query_values{
                {"http://", {}},
    });
    REQUIRE(u.fragment == "http://");
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
