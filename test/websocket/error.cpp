#include "websocket/error.hpp"

#include <optional>
#include <string>
#include <system_error>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "io/io.hpp"

namespace
{

using net::websocket::close_code;

}

TEST_CASE("close codes are errors, the peer's apart from those sent", "[websocket][error]")
{
    const std::error_code peer = close_code::protocol_error;
    const auto            sent = net::websocket::sent(close_code::protocol_error);

    CHECK(peer.category() == net::websocket::close_category());
    CHECK(peer.value() == 1002);
    CHECK(sent.category() == net::websocket::close_category());
    CHECK(sent.value() == net::websocket::sent_base + 1002);

    CHECK(peer == close_code::protocol_error);
    CHECK(sent != close_code::protocol_error);
    CHECK(net::websocket::from_peer(peer));
    CHECK_FALSE(net::websocket::from_peer(sent));
    CHECK(net::websocket::close_code_of(peer) == close_code::protocol_error);
    CHECK(net::websocket::close_code_of(sent) == close_code::protocol_error);

    CHECK(peer.message().contains("protocol error"));
    CHECK(peer.message().contains("from the peer"));
    CHECK(sent.message().contains("sent to the peer"));

    // codes for libraries and applications, as numbers.
    const std::error_code app = close_code{4000};
    CHECK(net::websocket::close_code_of(app) == close_code{4000});
    CHECK(app.message().contains("4000"));

    // other categories aren't closes.
    CHECK_FALSE(net::websocket::from_peer(std::make_error_code(std::errc::timed_out)));
    CHECK_FALSE(net::websocket::close_code_of(std::make_error_code(std::errc::timed_out)).has_value());
    CHECK_FALSE(net::websocket::from_peer(make_error_code(net::websocket::errc::bad_key)));
}

TEST_CASE("closes that only end the connection are io::status_condition::closed", "[websocket][error]")
{
    struct test_case
    {
        close_code code;
        bool       closed;
    };

    auto tc = GENERATE(test_case{.code = close_code::normal_closure, .closed = true},
                       test_case{.code = close_code::going_away, .closed = true},
                       test_case{.code = close_code::no_status_received, .closed = true},
                       test_case{.code = close_code::abnormal_closure, .closed = true},
                       test_case{.code = close_code::protocol_error, .closed = false},
                       test_case{.code = close_code::invalid_payload, .closed = false},
                       test_case{.code = close_code::message_too_big, .closed = false},
                       test_case{.code = close_code{3000}, .closed = false});

    CAPTURE(static_cast<int>(tc.code));

    const std::error_code peer = tc.code;
    CHECK((peer == net::io::status_condition::closed) == tc.closed);
    CHECK((net::websocket::sent(tc.code) == net::io::status_condition::closed) == tc.closed);
}

TEST_CASE("handshake failures are websocket::errc", "[websocket][error]")
{
    const std::error_code err = net::websocket::errc::origin_refused;
    CHECK(err.category() == net::websocket::category());
    CHECK(err == net::websocket::errc::origin_refused);
    CHECK_FALSE(err.message().empty());
}
