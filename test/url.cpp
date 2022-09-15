#include "url.hpp"

#include <catch.hpp>

#include <catch2/catch_message.hpp>

TEST_CASE("url::parse '/'", "[url][parse]")
{
    auto result = net::url::parse("/");

    REQUIRE(result.has_value());

    auto u = result.to_value();
    CHECK(u.scheme.empty());
    CHECK(u.userinfo.username.empty());
    CHECK(u.userinfo.password.empty());
    CHECK(u.host.empty());
    CHECK(u.port.empty());
    CHECK(u.path == "/");
    CHECK(u.query.empty());
    CHECK(u.fragment.empty());
}

TEST_CASE("url::parse 'foo/'", "[url][parse]")
{
    auto result = net::url::parse("foo/");

    REQUIRE(result.has_value());

    auto u = result.to_value();
    CHECK(u.scheme.empty());
    CHECK(u.userinfo.username.empty());
    CHECK(u.userinfo.password.empty());
    CHECK(u.host == "foo");
    CHECK(u.port.empty());
    CHECK(u.path == "/");
    CHECK(u.query.empty());
    CHECK(u.fragment.empty());
}

TEST_CASE("url::parse 'foo:9123/'", "[url][parse]")
{
    auto result = net::url::parse("foo:9123/");

    REQUIRE(result.has_value());

    auto u = result.to_value();
    CHECK(u.scheme == "foo");
    CHECK(u.userinfo.username.empty());
    CHECK(u.userinfo.password.empty());
    CHECK(u.host.empty());
    CHECK(u.port.empty());
    CHECK(u.path == "9123/");
    CHECK(u.query.empty());
    CHECK(u.fragment.empty());
}

TEST_CASE("url::parse 'http:///'", "[url][parse]")
{
    auto result = net::url::parse("http:///");

    REQUIRE(result.has_value());

    auto u = result.to_value();
    CHECK(u.scheme == "http");
    CHECK(u.userinfo.username.empty());
    CHECK(u.userinfo.password.empty());
    CHECK(u.host.empty());
    CHECK(u.port.empty());
    CHECK(u.path == "/");
    CHECK(u.query.empty());
    CHECK(u.fragment.empty());
}

TEST_CASE("url::parse 'http://foo/'", "[url][parse]")
{
    auto result = net::url::parse("http://foo/");

    REQUIRE(result.has_value());

    auto u = result.to_value();
    CHECK(u.scheme == "http");
    CHECK(u.userinfo.username.empty());
    CHECK(u.userinfo.password.empty());
    CHECK(u.host == "foo");
    CHECK(u.port.empty());
    CHECK(u.path == "/");
    CHECK(u.query.empty());
    CHECK(u.fragment.empty());
}

TEST_CASE("url::parse 'http://foo:9123/'", "[url][parse]")
{
    auto result = net::url::parse("http://foo:9123/");

    REQUIRE(result.has_value());

    auto u = result.to_value();
    CHECK(u.scheme == "http");
    CHECK(u.userinfo.username.empty());
    CHECK(u.userinfo.password.empty());
    CHECK(u.host == "foo");
    CHECK(u.port == "9123");
    CHECK(u.path == "/");
    CHECK(u.query.empty());
    CHECK(u.fragment.empty());
}

TEST_CASE("url::parse 'http://user:password@host:9123/path?foo=bar#fragment'", "[url][parse]")
{
    auto result = net::url::parse("http://user:password@host:9123/path?foo=bar#fragment");

    REQUIRE(result.has_value());

    auto u = result.to_value();
    CHECK(u.scheme == "http");
    CHECK(u.userinfo.username == "user");
    CHECK(u.userinfo.password == "password");
    CHECK(u.host == "host");
    CHECK(u.port == "9123");
    CHECK(u.path == "/path");
    CHECK(u.query
          == net::url::query_values{
              {"foo", {"bar"}}
    });
    CHECK(u.fragment == "fragment");
}

TEST_CASE("url::parse 'http://user:password@host:9123/path?foo=bar'", "[url][parse]")
{
    auto result = net::url::parse("http://user:password@host:9123/path?foo=bar");

    REQUIRE(result.has_value());

    auto u = result.to_value();
    CHECK(u.scheme == "http");
    CHECK(u.userinfo.username == "user");
    CHECK(u.userinfo.password == "password");
    CHECK(u.host == "host");
    CHECK(u.port == "9123");
    CHECK(u.path == "/path");
    CHECK(u.query
          == net::url::query_values{
              {"foo", {"bar"}}
    });
    CHECK(u.fragment.empty());
}

TEST_CASE("url::parse 'http://user:password@host:9123/path#fragment'", "[url][parse]")
{
    auto result = net::url::parse("http://user:password@host:9123/path#fragment");

    REQUIRE(result.has_value());

    auto u = result.to_value();
    CHECK(u.scheme == "http");
    CHECK(u.userinfo.username == "user");
    CHECK(u.userinfo.password == "password");
    CHECK(u.host == "host");
    CHECK(u.port == "9123");
    CHECK(u.path == "/path");
    CHECK(u.query.empty());
    CHECK(u.fragment == "fragment");
}

TEST_CASE("url::parse '/path?foo=bar&baz=xyzzy&qux=plugh'", "[url][parse]")
{
    auto result = net::url::parse("/path?foo=bar&baz=xyzzy&qux=plugh");

    REQUIRE(result.has_value());

    auto u = result.to_value();
    CHECK(u.scheme.empty());
    CHECK(u.userinfo.username.empty());
    CHECK(u.userinfo.password.empty());
    CHECK(u.host.empty());
    CHECK(u.port.empty());
    CHECK(u.path == "/path");
    CHECK(u.query
          == net::url::query_values{
              {"foo", {"bar"}  },
              {"baz", {"xyzzy"}},
              {"qux", {"plugh"}},
    });
    CHECK(u.fragment.empty());
}

TEST_CASE("url::parse '/path?foo=bar&foo=baz&foo=qux'", "[url][parse]")
{
    auto result = net::url::parse("/path?foo=bar&foo=baz&xyzzy=plugh&foo=qux");

    REQUIRE(result.has_value());

    auto u = result.to_value();
    CHECK(u.scheme.empty());
    CHECK(u.userinfo.username.empty());
    CHECK(u.userinfo.password.empty());
    CHECK(u.host.empty());
    CHECK(u.port.empty());
    CHECK(u.path == "/path");
    CHECK(u.query
          == net::url::query_values{
              {"foo",   {"bar", "baz", "qux"}},
              {"xyzzy", {"plugh"}            },
    });
    CHECK(u.fragment.empty());
}

// crazy weird url: https://daniel.haxx.se/blog/2022/09/08/http-http-http-http-http-http-http/
TEST_CASE("url::parse 'http://http://http://@http://http://?http://#http://'", "[url][parse]")
{
    auto result = net::url::parse("http://http://http://@http://http://?http://#http://");

    REQUIRE(result.has_value());

    auto u = result.to_value();

    CHECK(u.scheme == "http");
    CHECK(u.userinfo.username == "http");
    CHECK(u.userinfo.password == "//http://");
    CHECK(u.host == "http");
    CHECK(u.port.empty());
    CHECK(u.path == "//http://");
    CHECK(u.query
          == net::url::query_values{
              {"http://", {}},
    });
    CHECK(u.fragment == "http://");
}

TEST_CASE("url encoding 'foo%bar' <=> 'foo%25bar'", "[url][urlencoding]")
{
    constexpr auto input  = "foo%bar";
    constexpr auto expect = "foo%25bar";

    SECTION("encode")
    {
        auto result = net::url::encode(input);
        CHECK(result == expect);
    }

    SECTION("decode")
    {
        auto result = net::url::decode(expect);
        CHECK(result == input);
    }
}
