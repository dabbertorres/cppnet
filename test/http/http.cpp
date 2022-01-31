#include <catch2/catch.hpp>

#include <string>
#include <string_view>
#include <sstream>

#include "http/http.hpp"

using namespace net::http;

TEST_CASE("response header is added correctly") {
    std::basic_stringstream<uint8_t> stream;

    response resp{stream};

    resp.add_header("my-header", 5);
}
