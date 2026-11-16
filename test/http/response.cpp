#include "http/response.hpp"

#include <chrono>
#include <cstdint>
#include <exception> // IWYU pragma: keep
#include <string>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

namespace http = net::http;

TEST_CASE("format_date formats an HTTP date", "[http][response][date]")
{
    using namespace std::chrono_literals;
    using std::chrono::January;
    using std::chrono::November;
    using std::chrono::sys_days;

    // RFC 9110's example, and a time within a second of another.
    CHECK(http::format_date(sys_days{1994y / November / 6} + 8h + 49min + 37s) == "Sun, 06 Nov 1994 08:49:37 GMT");
    CHECK(http::format_date(sys_days{2026y / January / 1} + 999ms) == "Thu, 01 Jan 2026 00:00:00 GMT");
}

TEST_CASE("status classes cover their ranges, to their bounds", "[http][response][status]")
{
    struct test_case
    {
        std::uint16_t code = 0;
        std::string   kind = {};
    };

    const auto tc = GENERATE(test_case{.code = 100, .kind = "information"},
                             test_case{.code = 199, .kind = "information"},
                             test_case{.code = 200, .kind = "success"},
                             test_case{.code = 299, .kind = "success"},
                             test_case{.code = 300, .kind = "redirection"},
                             test_case{.code = 399, .kind = "redirection"},
                             test_case{.code = 400, .kind = "client error"},
                             test_case{.code = 499, .kind = "client error"},
                             test_case{.code = 500, .kind = "server error"},
                             test_case{.code = 599, .kind = "server error"});

    const auto s = static_cast<http::status>(tc.code);
    CAPTURE(tc.code);
    CHECK(http::status_is_information(s) == (tc.kind == "information"));
    CHECK(http::status_is_success(s) == (tc.kind == "success"));
    CHECK(http::status_is_redirection(s) == (tc.kind == "redirection"));
    CHECK(http::status_is_client_error(s) == (tc.kind == "client error"));
    CHECK(http::status_is_server_error(s) == (tc.kind == "server error"));
}
