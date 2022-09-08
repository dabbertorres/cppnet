#include "http/http11.hpp"

#include <sstream>

#include "http/http.hpp"
#include <catch.hpp>

using namespace net::http;

TEST_CASE("http 1.1 request parsing", "[http]")
{
    std::stringstream input{R"(GET /some/resource HTTP/1.1)"};
}
