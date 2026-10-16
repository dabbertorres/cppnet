#include "http/response.hpp"

#include <chrono>
#include <exception> // IWYU pragma: keep

#include <catch2/catch_test_macros.hpp>

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
