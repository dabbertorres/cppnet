#pragma once

#include <string>
#include <vector>

#include "http/headers.hpp"
#include "websocket/error.hpp" // IWYU pragma: export

// WebSockets (RFC 6455): a handler accepts one, and a program opens one (see docs/plans/websockets.md). So far, the
// handshake's options; the connection, accept() and connect() come in the plan's later stages.
namespace net::websocket
{

// accept_options are how a server accepts a WebSocket's handshake.
struct accept_options
{
    // subprotocols are the server's (Sec-WebSocket-Protocol), most preferred first: the first that the client offers
    // is chosen, compared exactly (as browsers compare the choice); none, if the client offers none of them.
    std::vector<std::string> subprotocols{};

    // origins allow handshakes from other origins than the request's own (Decided question 7). A request with an Origin
    // (as browsers send) is refused unless the origin's host and port are the request's own (its Host's, a port left
    // out being the origin's scheme's default), or match one of these: a host (ignoring case), each of whose labels may
    // be * for any one label (e.g. "*.example.com", which doesn't match "example.com" nor "a.b.example.com"),
    // optionally with a port, or :* for any (e.g. "localhost:3000"); without one, the origin's port must be its
    // scheme's default. A request without an Origin (e.g. a program's, not a browser's) isn't checked.
    //
    // The check is what keeps a page on another site from opening a WebSocket with the user's cookies (cross-site
    // WebSocket hijacking): browsers send cookies with cross-site WebSocket handshakes.
    std::vector<std::string> origins{};

    // insecure_any_origin skips the origin check: any origin is accepted. Only for endpoints that don't authenticate
    // with what browsers send by themselves (cookies, HTTP authentication, TLS client certificates).
    bool insecure_any_origin = false;
};

// connect_options are how a client opens a WebSocket.
struct connect_options
{
    // headers are sent with the handshake's request (e.g. Authorization, a Cookie), but for the handshake's own, which
    // replace any of theirs (Upgrade, Connection, and Sec-WebSocket-Key, -Version and -Protocol), and
    // Sec-WebSocket-Extensions, which is dropped: no extension is offered.
    http::headers headers{};

    // subprotocols are offered (Sec-WebSocket-Protocol), in order; the server may choose one of them, or none.
    std::vector<std::string> subprotocols{};

    // origin is sent as the request's Origin, if it's set (browsers send their page's; other programs, none).
    std::string origin{};
};

}
