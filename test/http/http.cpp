#include "http/http.hpp"

#include <sstream>
#include <string>
#include <string_view>

#include <catch.hpp>

using namespace net::http;

TEST_CASE("response header is added correctly")
{
    std::stringstream stream;

    /* response resp{stream}; */

    /* resp.add_header("my-header", 5); */
}
