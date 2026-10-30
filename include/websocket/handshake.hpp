#pragma once

#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

#include "http/headers.hpp"
#include "http/request.hpp"
#include "http/response.hpp"
#include "url.hpp"
#include "websocket/websocket.hpp"

// WebSockets' opening handshake (RFC 6455, section 4), as plain functions over HTTP's requests and responses: the
// server's checks of a request and the client's of a response, and what each sends. What accept() and connect() are
// built from, not for applications. Failures are websocket::errc.
namespace net::websocket::detail
{

// guid is RFC 6455's (section 1.3), which a key is hashed with.
inline constexpr std::string_view guid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

// accept_key is Sec-WebSocket-Accept for a request's Sec-WebSocket-Key: the base64 of the SHA-1 of the key and the
// GUID (RFC 6455, section 4.2.2).
[[nodiscard]] std::string accept_key(std::string_view key);

// check_request checks that req is a WebSocket handshake, returning its Sec-WebSocket-Key (a view of its headers). Over
// HTTP/1.1 (RFC 6455, section 4.2.1), it fails with:
// - not_websocket, for a method other than GET, a version before HTTP/1.1, a body (a Content-Length other than 0, or a
//   Transfer-Encoding), or without the token websocket in Upgrade, or upgrade in Connection (lists, ignoring case);
// - bad_version, unless Sec-WebSocket-Version is one line, 13 (the server answers 426, saying 13);
// - bad_key, unless Sec-WebSocket-Key is one line, of 16 bytes in base64 (strictly: 24 characters, padded, canonical).
// Over HTTP/2, the handshake is an extended CONNECT (RFC 8441, section 5), which the server presents as GET with
// protocol "websocket" (in any case): it has no key (an empty view is returned), nor Upgrade or Connection; it fails
// with not_websocket for any other request (or a body), and with bad_version as over HTTP/1.1.
[[nodiscard]] std::expected<std::string_view, std::error_code> check_request(const http::server_request& req);

// select_subprotocol is the first of the server's subprotocols that the request's Sec-WebSocket-Protocol offers (a
// list, over any number of lines), compared exactly; empty if it offers none of them (or none at all). It's a view of
// one of server's.
[[nodiscard]] std::string_view select_subprotocol(std::span<const std::string> server, const http::headers& request);

// check_origin checks req's Origin as options say (accept_options::origins, insecure_any_origin), failing with
// origin_refused for one that isn't allowed: one that isn't the request's own host and port, nor matches a pattern;
// "null" (an opaque origin, e.g. a sandboxed page's), or one that isn't an origin (scheme://host[:port]); or more than
// one line.
[[nodiscard]] std::error_code check_origin(const http::server_request& req, const accept_options& options);

// client_handshake is a client's handshake request, and the key it sends (to check the response's accept with).
struct client_handshake
{
    http::client_request request;
    std::string          key;
};

// make_request makes a client's handshake request for target (whose scheme is the caller's to have mapped, ws to http
// and wss to https), with a fresh random key: GET, HTTP/1.1, options' headers and then the handshake's (see
// connect_options::headers), the subprotocols offered (on one line) and the origin, if there are any.
[[nodiscard]] client_handshake make_request(const url& target, const connect_options& options);

// check_response checks a server's response to a handshake that sent key and offered the subprotocols offered,
// returning the subprotocol the server chose (a view of its headers; empty for none). It fails with:
// - bad_status, for a status other than 101 (Switching Protocols): redirects aren't followed, as browsers don't;
// - not_websocket, without the token websocket in Upgrade, or upgrade in Connection;
// - bad_accept, unless Sec-WebSocket-Accept is one line, accept_key(key) exactly;
// - bad_extension, for any extension in Sec-WebSocket-Extensions (none was offered);
// - bad_subprotocol, for a Sec-WebSocket-Protocol that isn't one line, one of offered (exactly).
// RFC 6455 (section 4.1) has the client fail the connection for any of them.
[[nodiscard]] std::expected<std::string_view, std::error_code>
check_response(const http::client_response& response, std::string_view key, std::span<const std::string> offered);

}
