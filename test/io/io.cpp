#include "io/io.hpp"

#include <cerrno>
#include <system_error>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("the end of the stream is closed, as are resets", "[io][errors]")
{
    const auto end = make_error_code(net::io::errc::end_of_stream);
    CHECK(end == net::io::status_condition::closed);
    CHECK(end != net::io::status_condition::timed_out);
    CHECK(end.message() == "end of stream");

    for (int err : {ECONNRESET, ECONNABORTED, EPIPE, ENOTCONN, ENETRESET})
    {
        CHECK(std::error_code{err, std::system_category()} == net::io::status_condition::closed);
        CHECK(std::error_code{err, std::generic_category()} == net::io::status_condition::closed);
    }

    CHECK(std::error_code{ECONNREFUSED, std::system_category()} != net::io::status_condition::closed);
    CHECK(make_error_code(net::io::errc::no_progress) != net::io::status_condition::closed);
    CHECK_FALSE(std::error_code{} == net::io::status_condition::closed);
}

TEST_CASE("timeouts compare with both conditions", "[io][errors]")
{
    const std::error_code err{ETIMEDOUT, std::system_category()};
    CHECK(err == net::io::status_condition::timed_out);
    CHECK(err == std::errc::timed_out);
    CHECK(err != net::io::status_condition::closed);
    CHECK(std::make_error_code(std::errc::timed_out) == net::io::status_condition::timed_out);
}

TEST_CASE("only the OS's codes match by their value", "[io][errors]")
{
    // a code in another category whose value happens to be ECONNRESET's.
    const std::error_code other{ECONNRESET, net::io::category()};
    CHECK(other != net::io::status_condition::closed);
}
