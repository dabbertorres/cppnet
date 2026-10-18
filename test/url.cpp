#include "url.hpp"

#include <exception> // IWYU pragma: keep
#include <expected>
#include <string>
#include <string_view>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_tostring.hpp>
#include <catch2/generators/catch_generators.hpp>

using namespace std::string_view_literals;

namespace Catch
{

template<>
struct StringMaker<net::url_parse_failure>
{
    static std::string convert(const net::url_parse_failure& value)
    {
        return std::string(net::url_parse_state_to_string(value.failed_at)) + ": " + std::to_string(value.index);
    }
};

template<typename T, typename E>
struct StringMaker<std::expected<T, E>>
{
    static std::string convert(const std::expected<T, E>& value)
    {
        return value.has_value() ? StringMaker<T>::convert(*value) : StringMaker<E>::convert(value.error());
    }
};

}

TEST_CASE("url::parse '/'", "[url][parse]")
{
    auto result = net::url::parse("/");

    CAPTURE(result);
    REQUIRE(result.has_value());

    auto u = result.value();
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

    CAPTURE(result);
    REQUIRE(result.has_value());

    auto u = result.value();
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

    CAPTURE(result);
    REQUIRE(result.has_value());

    auto u = result.value();
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

    CAPTURE(result);
    REQUIRE(result.has_value());

    auto u = result.value();
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

    CAPTURE(result);
    REQUIRE(result.has_value());

    auto u = result.value();
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

    CAPTURE(result);
    REQUIRE(result.has_value());

    auto u = result.value();
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

    CAPTURE(result);
    REQUIRE(result.has_value());

    auto u = result.value();
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

    CAPTURE(result);
    REQUIRE(result.has_value());

    auto u = result.value();
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

    CAPTURE(result);
    REQUIRE(result.has_value());

    auto u = result.value();
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

    CAPTURE(result);
    REQUIRE(result.has_value());

    auto u = result.value();
    CHECK(u.scheme.empty());
    CHECK(u.userinfo.username.empty());
    CHECK(u.userinfo.password.empty());
    CHECK(u.host.empty());
    CHECK(u.port.empty());
    CHECK(u.path == "/path");
    CHECK(u.query
          == net::url::query_values{
              {"foo",   {"bar"}},
              {"baz", {"xyzzy"}},
              {"qux", {"plugh"}},
    });
    CHECK(u.fragment.empty());
}

TEST_CASE("url::parse '/path?foo=bar&foo=baz&foo=qux'", "[url][parse]")
{
    auto result = net::url::parse("/path?foo=bar&foo=baz&xyzzy=plugh&foo=qux");

    CAPTURE(result);
    REQUIRE(result.has_value());

    auto u = result.value();
    CHECK(u.scheme.empty());
    CHECK(u.userinfo.username.empty());
    CHECK(u.userinfo.password.empty());
    CHECK(u.host.empty());
    CHECK(u.port.empty());
    CHECK(u.path == "/path");
    CHECK(u.query
          == net::url::query_values{
              {  "foo", {"bar", "baz", "qux"}},
              {"xyzzy",             {"plugh"}},
    });
    CHECK(u.fragment.empty());
}

// crazy weird url: https://daniel.haxx.se/blog/2022/09/08/http-http-http-http-http-http-http/
//
// The authority ends at the first '/' (RFC 3986, section 3.2), so the '@' after it is part of the path, not the end of
// userinfo. (Treating it as userinfo is what let "http://host/?email=a@b.com" connect to "b.com".)
TEST_CASE("url::parse 'http://http://http://@http://http://?http://#http://'", "[url][parse]")
{
    auto result = net::url::parse("http://http://http://@http://http://?http://#http://");

    CAPTURE(result);
    REQUIRE(result.has_value());

    auto u = result.value();

    CHECK(u.scheme == "http");
    CHECK(u.userinfo.empty());
    CHECK(u.host == "http");
    CHECK(u.port.empty());
    CHECK(u.path == "//http://@http://http://");
    CHECK(u.query
          == net::url::query_values{
              {"http://", {}},
    });
    CHECK(u.fragment == "http://");
}

TEST_CASE("url::parse splits at the right delimiters", "[url][parse]")
{
    struct test_case
    {
        std::string_view input;
        std::string_view scheme;
        std::string_view host;
        std::string_view port;
        std::string_view path;
    };

    auto tc = GENERATE(
        // a colon in a path isn't a scheme
        test_case{.input = "/a:b", .scheme = "", .host = "", .port = "", .path = "/a:b"},
        test_case{.input = "/v1/jobs/x:run", .scheme = "", .host = "", .port = "", .path = "/v1/jobs/x:run"},
        test_case{.input = "/search?q=a:b", .scheme = "", .host = "", .port = "", .path = "/search"},
        // an '@' after the authority isn't userinfo
        test_case{.input = "http://host/p?email=a@b.com", .scheme = "http", .host = "host", .port = "", .path = "/p"},
        test_case{.input = "http://host#a@b", .scheme = "http", .host = "host", .port = "", .path = ""},
        // IPv6
        test_case{.input = "http://[::1]:8080/x", .scheme = "http", .host = "::1", .port = "8080", .path = "/x"},
        test_case{.input = "http://[fe80::1]/", .scheme = "http", .host = "fe80::1", .port = "", .path = "/"},
        test_case{.input  = "http://[fe80::1%25en0]/",
                  .scheme = "http",
                  .host   = "fe80::1%25en0",
                  .port   = "",
                  .path   = "/"},
        test_case{.input  = "http://[::ffff:1.2.3.4]/",
                  .scheme = "http",
                  .host   = "::ffff:1.2.3.4",
                  .port   = "",
                  .path   = "/"},
        // no scheme or "//": starts with the host
        test_case{.input = "example.com/path", .scheme = "", .host = "example.com", .port = "", .path = "/path"},
        test_case{.input = "example.com", .scheme = "", .host = "example.com", .port = "", .path = ""},
        test_case{.input = "//example.com:81", .scheme = "", .host = "example.com", .port = "81", .path = ""},
        test_case{.input = "http://h:/", .scheme = "http", .host = "h", .port = "", .path = "/"});

    CAPTURE(tc.input);

    auto result = net::url::parse(tc.input);
    REQUIRE(result.has_value());

    auto u = result.value();
    CHECK(u.scheme == tc.scheme);
    CHECK(u.host == tc.host);
    CHECK(u.port == tc.port);
    CHECK(u.path == tc.path);
}

TEST_CASE("url::parse userinfo", "[url][parse]")
{
    auto result = net::url::parse("http://us%40er:p:w@host/");
    REQUIRE(result.has_value());

    auto u = result.value();
    CHECK(u.userinfo.username == "us@er");
    CHECK(u.userinfo.password == "p:w");
    CHECK(u.host == "host");
}

TEST_CASE("url::parse rejects invalid hosts and ports", "[url][parse]")
{
    struct test_case
    {
        std::string_view     input;
        net::url_parse_state failed_at;
    };

    auto tc = GENERATE(test_case{.input = "http://h:99999/", .failed_at = net::url_parse_state::port},
                       test_case{.input = "http://h:8o/", .failed_at = net::url_parse_state::port},
                       test_case{.input = "http://h:1:2/", .failed_at = net::url_parse_state::port},
                       test_case{.input = "http://[::1/", .failed_at = net::url_parse_state::host},
                       test_case{.input = "http://[::1]x/", .failed_at = net::url_parse_state::port},
                       test_case{.input = "http://[a\r\nX: y]/", .failed_at = net::url_parse_state::host},
                       test_case{.input = "http://[::1%25]/", .failed_at = net::url_parse_state::host},
                       test_case{.input = "http://[BeA%25e.n0]/", .failed_at = net::url_parse_state::host},
                       test_case{.input = "http://[1.2.3.4]/", .failed_at = net::url_parse_state::host},
                       // once decoded, these could end a Host header line, or be taken for another part of the URL
                       test_case{.input = "http://a%0d%0aX-Injected:%20y/", .failed_at = net::url_parse_state::host},
                       test_case{.input = "http://a%2fb/", .failed_at = net::url_parse_state::host},
                       test_case{.input = "http://a b/", .failed_at = net::url_parse_state::host});

    CAPTURE(tc.input);

    auto result = net::url::parse(tc.input);
    REQUIRE(!result.has_value());
    CHECK(result.error().failed_at == tc.failed_at);
}

TEST_CASE("url::parse query", "[url][parse]")
{
    auto result = net::url::parse("/?a=1&a=x+y%26z&b&&c=");
    REQUIRE(result.has_value());

    CHECK(result.value().query
          == net::url::query_values{
              {"a", {"1", "x y&z"}},
              {"b",             {}},
              {"c",           {""}},
    });
}

TEST_CASE("url::build", "[url][build]")
{
    SECTION("round trips")
    {
        auto input = GENERATE("http://user:pw@example.com:8080/a/b?k=v#frag"sv,
                              "https://example.com/"sv,
                              "http://[::1]:8080/x"sv,
                              "/just/a/path?q=1"sv,
                              "mailto:someone@example.com"sv);

        CAPTURE(input);

        auto parsed = net::url::parse(input);
        REQUIRE(parsed.has_value());
        CHECK(parsed.value().build() == input);
    }

    SECTION("encodes what would change the URL's meaning")
    {
        net::url u;
        u.scheme            = "http";
        u.host              = "example.com";
        u.path              = "/a b/c?d#e";
        u.query["k&"]       = {"v=1 2"};
        u.fragment          = "f g";
        u.userinfo.username = "u@s";

        auto built = u.build();
        CHECK(built == "http://u%40s@example.com/a%20b/c%3Fd%23e?k%26=v%3D1%202#f%20g");

        auto reparsed = net::url::parse(built);
        REQUIRE(reparsed.has_value());
        CHECK(reparsed.value() == u);
    }
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

TEST_CASE("url::decode", "[url][urlencoding]")
{
    using namespace std::string_view_literals;

    struct test_case
    {
        std::string_view input;
        std::string_view expect;
    };

    auto tc = GENERATE(test_case{.input = "", .expect = ""},
                       test_case{.input = "plain", .expect = "plain"},
                       test_case{.input = "%41%42", .expect = "AB"},
                       test_case{.input = "%2f%2F", .expect = "//"},
                       test_case{.input = "%00", .expect = "\0"sv},
                       test_case{.input = "%%41", .expect = "%A"},
                       // invalid escapes are kept as they are
                       test_case{.input = "%", .expect = "%"},
                       test_case{.input = "/%", .expect = "/%"},
                       test_case{.input = "abc%", .expect = "abc%"},
                       test_case{.input = "%4", .expect = "%4"},
                       test_case{.input = "%4g", .expect = "%4g"},
                       test_case{.input = "a%4gb", .expect = "a%4gb"},
                       test_case{.input = "abc%zzdef", .expect = "abc%zzdef"},
                       test_case{.input = "%-1", .expect = "%-1"});

    CAPTURE(tc.input);
    CHECK(net::url::decode(tc.input) == tc.expect);
}

TEST_CASE("url::parse keeps invalid escapes", "[url][parse]")
{
    auto result = net::url::parse("/a%zz/%");
    REQUIRE(result.has_value());
    CHECK(result.value().path == "/a%zz/%");
}
