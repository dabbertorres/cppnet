#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "coro/task.hpp"
#include "tcp.hpp"

namespace Botan::TLS
{
class Channel;
}

// A TLS 1.2 endpoint run by Botan directly, for what cppnet's own never do (renegotiate), for the tests of how cppnet
// answers.
namespace test
{

// alert_seen is an alert a botan_peer received: its TLS code (e.g. 100 for no_renegotiation) and level.
struct alert_seen
{
    std::uint8_t code  = 0;
    bool         fatal = false;

    bool operator==(const alert_seen&) const = default;
};

// botan_peer is a TLS 1.2 client or server over a connection cppnet made, using Botan with its default policy (but
// for TLS 1.3, which it doesn't offer). Its steps are coroutines, which one task at a time awaits in turn, each
// returning what failed (empty if nothing did: a socket's error, or what Botan threw). Its socket's reads and writes
// have 5 s altogether.
class botan_peer
{
public:
    // client starts a client's handshake over conn, offering alpn. NOTE: it doesn't verify the server's certificate:
    //        what's tested follows the handshake.
    static std::unique_ptr<botan_peer> client(net::tcp_socket conn, const std::vector<std::string>& alpn = {});

    // server is a server over conn, with the certificate chain and key in chain and key.
    static std::unique_ptr<botan_peer>
    server(net::tcp_socket conn, const std::filesystem::path& chain, const std::filesystem::path& key);

    // NOTE: not movable either, as Botan's callbacks point to it.
    botan_peer(const botan_peer&)            = delete;
    botan_peer(botan_peer&&)                 = delete;
    botan_peer& operator=(const botan_peer&) = delete;
    botan_peer& operator=(botan_peer&&)      = delete;
    ~botan_peer();

    // handshake sends and receives the handshake's messages until it's done.
    net::coro::task<std::string> handshake();

    // send sends data, as application data.
    net::coro::task<std::string> send(std::string_view data);

    // receive reads until received holds at least size bytes.
    net::coro::task<std::string> receive(std::size_t size);

    // renegotiate starts a new handshake on the connection: a client's ClientHello, or a server's HelloRequest.
    net::coro::task<std::string> renegotiate();

    // read_to_end reads (and answers what Botan answers) until the socket's reads fail, returning why: e.g.
    // io::errc::end_of_stream, once the peer has closed the connection.
    net::coro::task<std::error_code> read_to_end();

    // what's been received as application data.
    std::string received;

    // the first alert received that wasn't close_notify, if any.
    std::optional<alert_seen> alert;

    // the version of the session established (its wire value, e.g. 0x0303), once one has been.
    std::uint16_t version = 0;

    // what Botan threw (or what failed setting it up, or a write's error), once something has.
    std::string failure;

private:
    struct callbacks;

    explicit botan_peer(net::tcp_socket conn);

    // flush writes what Botan has for the socket.
    net::coro::task<std::string> flush();

    // read_once reads from the socket once, passing what it read to Botan and writing what it answers, returning the
    // read's error (and what Botan threw in failure).
    net::coro::task<std::error_code> read_once();

    net::tcp_socket                      conn;
    std::shared_ptr<callbacks>           cb;
    std::unique_ptr<Botan::TLS::Channel> channel;
};

}
