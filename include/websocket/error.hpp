#pragma once

#include <cstdint>
#include <optional>
#include <system_error>
#include <type_traits>

// WebSockets' errors (RFC 6455): how a WebSocket ended, as close codes, and why a handshake failed.
//
// A WebSocket's end is an error in close_category():
// - the code of the close the peer sent (e.g. 1000 for normal_closure; no_status_received, 1005, for a close without a
//   code), or abnormal_closure (1006) for its connection ending without one: err == close_code::going_away is the
//   peer's going away;
// - or the code of a close this side sent, for what it found wrong with what the peer sent (protocol_error,
//   invalid_payload, message_too_big) or because it was asked to (e.g. normal_closure, going_away): as sent_base plus
//   the code, so it isn't mistaken for the peer's (sent() makes one).
// from_peer() says which side's it is, and close_code_of() what its code is, whichever side's. Those that mean the
// connection just ended (normal_closure, going_away, no_status_received and abnormal_closure, from either side) are
// equivalent to io::status_condition::closed, so code that only asks whether the stream is over needn't know about
// WebSockets. Handshake failures are errc, in category().
namespace net::websocket
{

// close_code is a close's status code (RFC 6455, section 7.4, and IANA's registry): those named here, and 3000 to 4999
// (registered for libraries and frameworks, and private use) as numbers, e.g. close_code{4000}.
enum class close_code : std::uint16_t
{
    normal_closure   = 1000, // the purpose for which the connection was made has been fulfilled
    going_away       = 1001, // e.g. a server going down, or a browser leaving the page
    protocol_error   = 1002, // the peer broke the protocol
    unsupported_data = 1003, // a type of data the endpoint can't accept (e.g. binary, for one that takes text only)
    // no_status_received is a close without a code. Never sent as a code.
    no_status_received = 1005,
    // abnormal_closure is the connection ending without a close. Never sent as a code.
    abnormal_closure    = 1006,
    invalid_payload     = 1007, // a message's data wasn't its type's (e.g. a text message that isn't UTF-8)
    policy_violation    = 1008, // a message that violates the endpoint's policy (a generic code, for no other)
    message_too_big     = 1009, // a message too big to process
    mandatory_extension = 1010, // a client's: the server didn't agree on an extension the client needs
    internal_error      = 1011, // a server's: something unexpected kept it from fulfilling the request
    service_restart     = 1012, // a server's: it's restarting
    try_again_later     = 1013, // a server's: it's overloaded, for now
    bad_gateway         = 1014, // a gateway's: the server it's for answered badly
};

// sent_base is added to a close's code for an error that this side sent the close for.
inline constexpr int sent_base = 10'000;

// close_category is WebSockets' ends: the peer's close codes, and those this side sent (at sent_base).
[[nodiscard]] const std::error_category& close_category() noexcept;

// make_error_code is the error for the peer's close (or its connection's end, abnormal_closure) with code.
[[nodiscard]] std::error_code make_error_code(close_code code) noexcept;

// sent is the error for a close with code that this side sent.
[[nodiscard]] std::error_code sent(close_code code) noexcept;

// from_peer is whether err is a close the peer sent (or its connection ending without one), rather than one sent to
// it.
[[nodiscard]] bool from_peer(std::error_code err) noexcept;

// close_code_of is err's close code, whichever side sent it; none if err isn't in close_category().
[[nodiscard]] std::optional<close_code> close_code_of(std::error_code err) noexcept;

// errc are the handshake's failures.
enum class errc : std::uint8_t
{
    // the request (or response) isn't a WebSocket handshake: e.g. not a GET, or without Upgrade: websocket or
    // Connection: upgrade.
    not_websocket = 1,
    // the request's Sec-WebSocket-Version isn't 13 (the server answers 426, saying 13).
    bad_version = 2,
    // the request's Sec-WebSocket-Key isn't one key of 16 bytes, in base64.
    bad_key = 3,
    // the request's Origin isn't allowed (the server answers 403).
    origin_refused = 4,
    // the server answered with a status other than 101 (Switching Protocols).
    bad_status = 5,
    // the server's Sec-WebSocket-Accept isn't the one for the key sent.
    bad_accept = 6,
    // the server chose a subprotocol the client didn't offer (or more than one).
    bad_subprotocol = 7,
    // the server chose an extension the client didn't offer (it offers none).
    bad_extension = 8,
};

[[nodiscard]] const std::error_category& category() noexcept;

[[nodiscard]] std::error_code make_error_code(errc err) noexcept;

}

template<>
struct std::is_error_code_enum<net::websocket::close_code> : std::true_type
{};

template<>
struct std::is_error_code_enum<net::websocket::errc> : std::true_type
{};
