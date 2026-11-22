#include "exception.hpp"

#include <cerrno>
#include <string_view>
#include <system_error>

#include <netdb.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

TEST_CASE("errno_code is an OS error", "[exception][errno_code]")
{
    const auto err = GENERATE(ENOENT, ECONNRESET, EAGAIN, EBADF);

    const auto code = net::errno_code(err);
    CHECK(code.value() == err);
    CHECK(code.category() == std::system_category());
    CHECK(code == std::errc{err});
}

TEST_CASE("errno_code reads errno by default", "[exception][errno_code]")
{
    errno = ECONNREFUSED;
    CHECK(net::errno_code() == net::errno_code(ECONNREFUSED));
    CHECK(net::errno_code() == std::errc::connection_refused);
}

TEST_CASE("system_error_from_errno carries the OS error and what failed", "[exception][errno_code]")
{
    const auto err = net::system_error_from_errno(EACCES, "failed to open");
    CHECK(err.code() == net::errno_code(EACCES));
    CHECK(std::string_view{err.what()}.starts_with("failed to open"));
}

TEST_CASE("make_gai_error_code reports EAI_SYSTEM as errno", "[exception][errno_code]")
{
    errno = EMFILE;
    CHECK(net::make_gai_error_code(EAI_SYSTEM) == net::errno_code(EMFILE));
}
