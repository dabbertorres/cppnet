#include "http/http.hpp"

#include <exception> // IWYU pragma: keep
#include <sstream>

#include <catch.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("response header is added correctly")
{
    std::stringstream stream;

    /* response resp{stream}; */

    /* resp.add_header("my-header", 5); */
}
