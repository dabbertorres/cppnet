#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include "coro/task.hpp"
#include "listen.hpp"

// A TLS 1.2 server without the Extended Master Secret extension (RFC 7627), as older servers are, for the client's
// tests: cppnet's own server always agrees on it, as Botan does.
namespace test
{

// no_ems_outcome is how a connection to serve_without_ems() went, as its server saw it.
struct no_ems_outcome
{
    // whether the client's ClientHello offered Extended Master Secret.
    bool client_offered_ems = false;

    // whether the handshake completed, and then, as Botan's summary of the session established says, whether it used
    // Extended Master Secret (never, if the server works: it derives the master secret from what its ServerHello said)
    // and the version agreed on (its wire value, e.g. 0x0303).
    bool          handshake_done = false;
    bool          used_ems       = false;
    std::uint16_t version        = 0;

    // what the server echoed back.
    std::string echoed;

    // the alert the client sent, if any (its TLS code, e.g. 40 for handshake_failure).
    std::optional<std::uint8_t> client_alert;

    // why the connection failed on the server's side (Botan's message, or a socket's error), if it did.
    std::string failure;
};

// serve_without_ems accepts a connection on listener and serves TLS 1.2 on it, with chain and key, using Botan directly
// (cppnet's server can't leave the extension out): its ServerHello drops the Extended Master Secret extension, whether
// or not the client offers it. It echoes what it reads until the client closes (answering its close_notify), and
// records how it went in out. Its socket's reads and writes have 5 s altogether.
net::coro::task<>
serve_without_ems(net::listener* listener, std::filesystem::path chain, std::filesystem::path key, no_ems_outcome* out);

}
