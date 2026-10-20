#include "url.hpp"

#include <exception> // IWYU pragma: keep
#include <expected>
#include <string>
#include <string_view>
#include <system_error>

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
    CHECK(u.query == "foo=bar");
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
    CHECK(u.query == "foo=bar");
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
    CHECK(u.query == "foo=bar&baz=xyzzy&qux=plugh");
    CHECK(net::url::parse_query(u.query)
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
    CHECK(u.query == "foo=bar&foo=baz&xyzzy=plugh&foo=qux");
    CHECK(net::url::parse_query(u.query)
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
    CHECK(u.query == "http://");
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

    CHECK(result.value().query == "a=1&a=x+y%26z&b&&c=");
    CHECK(net::url::parse_query(result.value().query)
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
                              "mailto:someone@example.com"sv,
                              // encodings, and the query's order, are kept as written
                              "https://example.com/a%2Fb%2fc/%41?z=1&a=x+y%2Bz&a=%2f&s=ab+c/d=#f%2F"sv);

        CAPTURE(input);

        auto parsed = net::url::parse(input);
        REQUIRE(parsed.has_value());
        CHECK(parsed.value().build() == input);
    }

    SECTION("encodes what can't appear unencoded")
    {
        net::url u;
        u.scheme            = "http";
        u.host              = "example.com";
        u.path              = "/a b/c?d#e%";
        u.query             = "k=" + net::url::encode("v&=1 2") + "&x=a b#";
        u.fragment          = "f g";
        u.userinfo.username = "u@s";

        auto built = u.build();
        CHECK(built == "http://u%40s@example.com/a%20b/c%3Fd%23e%25?k=v%26%3D1%202&x=a%20b%23#f%20g");

        // which parses as the parts encoded as build() encodes them.
        auto reparsed = net::url::parse(built);
        REQUIRE(reparsed.has_value());
        CHECK(reparsed->userinfo == u.userinfo);
        CHECK(reparsed->path == "/a%20b/c%3Fd%23e%25");
        CHECK(reparsed->query == "k=v%26%3D1%202&x=a%20b%23");
        CHECK(reparsed->fragment == "f%20g");
        CHECK(reparsed->build() == built);
    }

    SECTION("an empty query or fragment is none")
    {
        auto parsed = net::url::parse("http://example.com/p?#");
        REQUIRE(parsed.has_value());
        CHECK(parsed->query.empty());
        CHECK(parsed->fragment.empty());
        CHECK(parsed->build() == "http://example.com/p");
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

TEST_CASE("url::parse keeps the path, query and fragment as written", "[url][parse]")
{
    struct test_case
    {
        std::string_view input;
        std::string_view path;
        std::string_view query;
        std::string_view fragment;
    };

    auto tc = GENERATE(
        // encodings are kept as written, whichever case their hex digits are in, even of unreserved characters.
        test_case{.input = "/a%2Fb%2fc/%41%7e", .path = "/a%2Fb%2fc/%41%7e", .query = "", .fragment = ""},
        test_case{.input    = "/?Expires=1&Signature=ab+c/d=&Key-Pair-Id=K%2F2",
                  .path     = "/",
                  .query    = "Expires=1&Signature=ab+c/d=&Key-Pair-Id=K%2F2",
                  .fragment = ""},
        test_case{.input = "/p?q=a?b#f/?%23", .path = "/p", .query = "q=a?b", .fragment = "f/?%23"},
        // what can't appear unencoded is encoded: a '%' that doesn't start an encoding, spaces, controls and
        // non-ASCII bytes, and characters RFC 3986 doesn't allow in that part.
        test_case{.input = "/a%zz/%", .path = "/a%25zz/%25", .query = "", .fragment = ""},
        test_case{.input = "/a%4", .path = "/a%254", .query = "", .fragment = ""},
        test_case{.input = "/a b?c d#e f", .path = "/a%20b", .query = "c%20d", .fragment = "e%20f"},
        test_case{.input = "/\x01\xC3\xA9", .path = "/%01%C3%A9", .query = "", .fragment = ""},
        test_case{.input    = "/a|b?x[0]={\"y\"}#^",
                  .path     = "/a%7Cb",
                  .query    = "x%5B0%5D=%7B%22y%22%7D",
                  .fragment = "%5E"},
        test_case{.input = "/p#a#b", .path = "/p", .query = "", .fragment = "a%23b"});

    CAPTURE(tc.input);

    auto result = net::url::parse(tc.input);
    REQUIRE(result.has_value());
    CHECK(result->path == tc.path);
    CHECK(result->query == tc.query);
    CHECK(result->fragment == tc.fragment);
}

TEST_CASE("url::origin_form is the path and query as they are", "[url][origin_form]")
{
    auto parsed = net::url::parse("https://example.com/a%2Fb%2fc?z=1&a=x+y%2Bz&s=ab+c/d=#frag");
    REQUIRE(parsed.has_value());
    CHECK(parsed->origin_form() == "/a%2Fb%2fc?z=1&a=x+y%2Bz&s=ab+c/d=");

    // nothing set in them can end a request line.
    net::url u;
    u.path  = "a b\r\n";
    u.query = "k=v\r\nX: y";
    CHECK(u.origin_form() == "/a%20b%0D%0A?k=v%0D%0AX:%20y");

    CHECK(net::url{}.origin_form() == "/");
}

namespace
{

// RFC 3986, section 5.4's base.
constexpr auto rfc_base = "http://a/b/c/d;p?q"sv;

std::string resolved(std::string_view base, std::string_view reference)
{
    auto parsed = net::url::parse(base);
    REQUIRE(parsed.has_value());

    auto result = net::url::resolve(*parsed, reference);
    if (!result.has_value()) return "error: " + result.error().message();
    return result->build();
}

}

TEST_CASE("url::resolve resolves RFC 3986's normal examples", "[url][resolve]")
{
    struct test_case
    {
        std::string_view reference;
        std::string_view expect;
    };

    // section 5.4.1
    auto tc = GENERATE(test_case{.reference = "g:h", .expect = "g:h"},
                       test_case{.reference = "g", .expect = "http://a/b/c/g"},
                       test_case{.reference = "./g", .expect = "http://a/b/c/g"},
                       test_case{.reference = "g/", .expect = "http://a/b/c/g/"},
                       test_case{.reference = "/g", .expect = "http://a/g"},
                       test_case{.reference = "//g", .expect = "http://g"},
                       test_case{.reference = "?y", .expect = "http://a/b/c/d;p?y"},
                       test_case{.reference = "g?y", .expect = "http://a/b/c/g?y"},
                       test_case{.reference = "#s", .expect = "http://a/b/c/d;p?q#s"},
                       test_case{.reference = "g#s", .expect = "http://a/b/c/g#s"},
                       test_case{.reference = "g?y#s", .expect = "http://a/b/c/g?y#s"},
                       test_case{.reference = ";x", .expect = "http://a/b/c/;x"},
                       test_case{.reference = "g;x", .expect = "http://a/b/c/g;x"},
                       test_case{.reference = "g;x?y#s", .expect = "http://a/b/c/g;x?y#s"},
                       test_case{.reference = "", .expect = "http://a/b/c/d;p?q"},
                       test_case{.reference = ".", .expect = "http://a/b/c/"},
                       test_case{.reference = "./", .expect = "http://a/b/c/"},
                       test_case{.reference = "..", .expect = "http://a/b/"},
                       test_case{.reference = "../", .expect = "http://a/b/"},
                       test_case{.reference = "../g", .expect = "http://a/b/g"},
                       test_case{.reference = "../..", .expect = "http://a/"},
                       test_case{.reference = "../../", .expect = "http://a/"},
                       test_case{.reference = "../../g", .expect = "http://a/g"});

    CAPTURE(tc.reference);
    CHECK(resolved(rfc_base, tc.reference) == tc.expect);
}

TEST_CASE("url::resolve resolves RFC 3986's abnormal examples", "[url][resolve]")
{
    struct test_case
    {
        std::string_view reference;
        std::string_view expect;
    };

    // section 5.4.2 (a strict parser's, for "http:g").
    auto tc = GENERATE(test_case{.reference = "../../../g", .expect = "http://a/g"},
                       test_case{.reference = "../../../../g", .expect = "http://a/g"},
                       test_case{.reference = "/./g", .expect = "http://a/g"},
                       test_case{.reference = "/../g", .expect = "http://a/g"},
                       test_case{.reference = "g.", .expect = "http://a/b/c/g."},
                       test_case{.reference = ".g", .expect = "http://a/b/c/.g"},
                       test_case{.reference = "g..", .expect = "http://a/b/c/g.."},
                       test_case{.reference = "..g", .expect = "http://a/b/c/..g"},
                       test_case{.reference = "./../g", .expect = "http://a/b/g"},
                       test_case{.reference = "./g/.", .expect = "http://a/b/c/g/"},
                       test_case{.reference = "g/./h", .expect = "http://a/b/c/g/h"},
                       test_case{.reference = "g/../h", .expect = "http://a/b/c/h"},
                       test_case{.reference = "g;x=1/./y", .expect = "http://a/b/c/g;x=1/y"},
                       test_case{.reference = "g;x=1/../y", .expect = "http://a/b/c/y"},
                       test_case{.reference = "g?y/./x", .expect = "http://a/b/c/g?y/./x"},
                       test_case{.reference = "g?y/../x", .expect = "http://a/b/c/g?y/../x"},
                       test_case{.reference = "g#s/./x", .expect = "http://a/b/c/g#s/./x"},
                       test_case{.reference = "g#s/../x", .expect = "http://a/b/c/g#s/../x"},
                       test_case{.reference = "http:g", .expect = "http:g"});

    CAPTURE(tc.reference);
    CHECK(resolved(rfc_base, tc.reference) == tc.expect);
}

TEST_CASE("url::resolve keeps encodings as written", "[url][resolve]")
{
    struct test_case
    {
        std::string_view base;
        std::string_view reference;
        std::string_view expect;
    };

    auto tc =
        GENERATE(test_case{.base      = "https://cdn.example/a/b",
                           .reference = "c%2Fd/e%2f?Expires=1&Signature=ab+c/d=&Key=K%2F2#x%2F",
                           .expect    = "https://cdn.example/a/c%2Fd/e%2f?Expires=1&Signature=ab+c/d=&Key=K%2F2#x%2F"},
                 test_case{.base = "http://h/a%2Fb/c?q=%2f", .reference = "", .expect = "http://h/a%2Fb/c?q=%2f"},
                 test_case{.base = "http://h/a%2Fb/c?q", .reference = "d", .expect = "http://h/a%2Fb/d"},
                 // only "." and ".." are dot segments, not their encodings.
                 test_case{.base = "http://h/a/b/c", .reference = "%2E%2e/d", .expect = "http://h/a/b/%2E%2e/d"},
                 // what can't appear unencoded is encoded, as url::parse() encodes it.
                 test_case{.base = "http://h/a/", .reference = "b c?d e#f g", .expect = "http://h/a/b%20c?d%20e#f%20g"},
                 test_case{.base = "http://h/a/", .reference = "%zz", .expect = "http://h/a/%25zz"});

    CAPTURE(tc.base, tc.reference);
    CHECK(resolved(tc.base, tc.reference) == tc.expect);
}

TEST_CASE("url::resolve reads the reference strictly", "[url][resolve]")
{
    struct test_case
    {
        std::string_view base;
        std::string_view reference;
        std::string_view expect;
    };

    auto tc = GENERATE(
        // a relative path, not a host (as url::parse() would read "next/page").
        test_case{.base = "http://h/a/b", .reference = "next/page", .expect = "http://h/a/next/page"},
        test_case{.base = "http://h/a/b", .reference = "example.com", .expect = "http://h/a/example.com"},
        // another scheme or authority replaces the base's.
        test_case{.base = "http://u:p@h:81/a?q#f", .reference = "//g/x", .expect = "http://g/x"},
        test_case{.base = "http://h/a", .reference = "https://g:8443/./x/../y", .expect = "https://g:8443/y"},
        test_case{.base = "http://h/a", .reference = "HTTP://g", .expect = "HTTP://g"},
        test_case{.base = "http://u@h:81/a/b", .reference = "c?x#y", .expect = "http://u@h:81/a/c?x#y"},
        // a base with an authority but no scheme, as url::parse() reads "h/a".
        test_case{.base = "h/a/b", .reference = "../c", .expect = "//h/c"},
        test_case{.base = "http://h", .reference = "g", .expect = "http://h/g"},
        // "?" alone is an empty query, replacing the base's.
        test_case{.base = "http://h/a?q", .reference = "?", .expect = "http://h/a"},
        test_case{.base = "http://h/a?q#f", .reference = "#", .expect = "http://h/a?q"},
        // a path that would start with "//" keeps an (empty) authority before it.
        test_case{.base = "g:a/b", .reference = "..//x", .expect = "g:////x"});

    CAPTURE(tc.base, tc.reference);
    CHECK(resolved(tc.base, tc.reference) == tc.expect);
}

TEST_CASE("url::resolve fails for invalid references", "[url][resolve]")
{
    auto reference = GENERATE(
        // a relative path's first segment can't have a ':', as it would be a scheme.
        "1a:b"sv,
        ":x"sv,
        "a b:c/d"sv,
        // as url::parse(), an invalid host or port.
        "//h:99999/"sv,
        "//[::1/"sv,
        "//a%2fb/"sv,
        "http://h:x/"sv);

    CAPTURE(reference);

    auto base = net::url::parse(rfc_base);
    REQUIRE(base.has_value());

    auto result = net::url::resolve(*base, reference);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == std::errc::invalid_argument);
}

TEST_CASE("url::resolve needs a base, and makes a url, with a scheme or an authority", "[url][resolve]")
{
    auto base = net::url::parse("/a/b");
    REQUIRE(base.has_value());

    auto result = net::url::resolve(*base, "c");
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == std::errc::invalid_argument);

    // nor can it make one: an empty authority without a scheme.
    auto without_scheme = net::url::parse("h/a");
    REQUIRE(without_scheme.has_value());
    CHECK(net::url::resolve(*without_scheme, "//")
          == std::unexpected(std::make_error_code(std::errc::invalid_argument)));
    CHECK(net::url::resolve(*without_scheme, "//g").has_value());
}
