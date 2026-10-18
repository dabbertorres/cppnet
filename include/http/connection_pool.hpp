#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>

#include "coro/task.hpp"
#include "http/connection.hpp"
#include "http/http2_client.hpp"
#include "io/scheduler.hpp"

#ifdef NET_HAS_TLS
#    include "tls/tls.hpp"
#endif

namespace net::http
{

struct client_options
{
    // How long to wait for a connection: both to connect, and for one to be free when at a limit.
    std::chrono::milliseconds connect_timeout = std::chrono::seconds{10};

    // Whether to turn on TCP keepalives (SO_KEEPALIVE), which detect connections to hosts that have gone away.
    bool tcp_keepalive = true;

    // Whether to reuse connections for more than one request (HTTP keep-alive).
    bool keep_alive = true;

    // The most connections open at once (in use, or idle), to each host, and altogether. 0 is no limit. A request waits
    // (up to connect_timeout) for a connection to be free when at a limit.
    std::size_t max_connections_per_host = 0;
    std::size_t max_connections          = 0;

    // How long a connection is kept idle, waiting to be reused, before it's closed.
    std::chrono::milliseconds idle_timeout = std::chrono::seconds{90};

    // How long to wait for a response's head, once its request has been sent. 0 is no limit.
    std::chrono::milliseconds response_header_timeout = std::chrono::seconds{30};

    // How long sending a request, or reading a response's body, may go without progress: each read or write of the
    // connection may wait this long for it. Slow but steady transfers aren't limited. 0 is no limit.
    std::chrono::milliseconds io_timeout = std::chrono::seconds{30};

    // Whether to speak HTTP/2 (each request a stream of a connection that others share) to servers that do: over TLS,
    // it's offered with ALPN, and used if the server agrees.
    bool http2 = true;

    // Whether to speak HTTP/2 to http:// URLs, without asking first ("prior knowledge"): only for servers known to.
    bool http2_prior_knowledge = false;

    // Whether a request waits for an HTTP/2 connection's stream to be free when the server's MAX_CONCURRENT_STREAMS
    // are open (as Chromium, hyper and gRPC do), rather than opening another connection (as Go, curl and OkHttp do,
    // within max_connections_per_host).
    bool strict_max_concurrent_streams = false;

#ifdef NET_HAS_TLS
    // For https:// URLs: which certificate authorities to trust (the system's, by default), or not to verify servers'
    // certificates, for testing. Its ALPN protocols are ignored: the client offers h2 (if http2 is on) and http/1.1.
    tls::client_options tls{};

    // How long a TLS handshake may take, once connected (separately from connect_timeout, so an error says which
    // took too long). 0 is no limit.
    std::chrono::milliseconds tls_handshake_timeout = std::chrono::seconds{10};
#endif
};

struct connection_stats
{
    std::size_t open = 0; // in use, idle, or connecting
    std::size_t idle = 0;

    bool operator==(const connection_stats&) const = default;
};

namespace detail
{

    // connection_pool keeps connections open for reuse, keyed by scheme, host and port, within client_options' limits.
    //
    // It's shared (by a client, its copies, and the responses whose bodies are using connections), and closes its idle
    // connections when the last of them goes. Connections must be released (or their leases destroyed), even if they
    // aren't reusable, so that their places are free for others.
    class connection_pool : public std::enable_shared_from_this<connection_pool>
    {
    public:
        // lease is an HTTP/1.x connection for one request (conn), or an HTTP/2 connection the request is to take a
        // stream of (http2).
        struct lease
        {
            std::unique_ptr<connection>              conn;
            std::shared_ptr<http2_client_connection> http2;
            std::string                              key;
            bool reused = false; // it was idle in the pool, rather than newly connected
        };

        connection_pool(io::scheduler* scheduler, client_options options);

        connection_pool(const connection_pool&)            = delete;
        connection_pool& operator=(const connection_pool&) = delete;
        connection_pool(connection_pool&&)                 = delete;
        connection_pool& operator=(connection_pool&&)      = delete;

        ~connection_pool();

        // acquire returns an idle connection to host (if allow_idle, and one is still open), or connects a new one if
        // the limits allow, waiting for one to be free if not, and for https, runs a TLS handshake over it. It fails
        // with std::errc::timed_out if connecting takes longer than connect_timeout (or the handshake
        // tls_handshake_timeout), as tcp_socket::connect does, or with TLS's errors (e.g. a certificate that couldn't
        // be verified). scheme is "http" or "https" (or with NET_HAS_TLS undefined, only "http").
        coro::task<std::expected<lease, std::error_code>>
        acquire(std::string scheme, std::string host, std::string port, bool allow_idle);

        // release returns a leased connection: kept idle if it's reusable (it's at the start of a response, with
        // nothing unread), and closed otherwise.
        void release(lease&& leased, bool reusable) noexcept;

        // close_idle closes every idle connection.
        void close_idle() noexcept;

        [[nodiscard]] connection_stats stats() const;
        // stats are the connections to host and port, over any scheme.
        [[nodiscard]] connection_stats stats(const std::string& host, const std::string& port) const;

        [[nodiscard]] const client_options& options() const noexcept { return config; }

    private:
        using connections = std::vector<std::unique_ptr<connection>>;

        struct idle_connection
        {
            std::unique_ptr<connection> conn;
            io::clock::time_point       expires;
        };

        struct host_connections
        {
            std::size_t                                           open = 0;
            std::deque<idle_connection>                           idle; // oldest first
            std::vector<std::shared_ptr<http2_client_connection>> http2;
        };

        static std::string make_key(const std::string& scheme, const std::string& host, const std::string& port);

        // connect connects to host, within deadline, and for https, runs a TLS handshake.
        coro::task<std::expected<std::unique_ptr<connection>, std::error_code>>
        connect(bool tls, std::string host, std::string port, io::clock::time_point deadline);

        // wants_http2 is whether a new connection is to speak HTTP/2: it agreed on h2 (or it's cleartext, with prior
        // knowledge).
        [[nodiscard]] bool wants_http2(const connection& conn) const noexcept;

        // start_http2 makes a connection an HTTP/2 connection the host's requests share, returning nullptr if the
        // scheduler's stopping.
        std::shared_ptr<http2_client_connection> start_http2(const std::string& key, std::unique_ptr<connection> conn);

        // available_http2 is one of the host's HTTP/2 connections with a stream free, which it reserves for the
        // request, if there is one (holding mu).
        std::shared_ptr<http2_client_connection> available_http2(const std::string& key);

        // idle_count is how many of a host's connections are idle (holding mu).
        static std::size_t idle_count(const host_connections& conns);

        // reap closes idle connections as they expire, until there are none.
        static coro::task<> reap(std::weak_ptr<connection_pool> weak, io::scheduler* scheduler);

        // claim is what try_claim found for a request: a connection to use, or room to connect one, or neither (so it
        // waits).
        struct claim
        {
            std::optional<lease> leased{};
            bool                 connect = false;
        };

        // These must be called while holding mu. Connections to close are moved to closing, to close after unlocking.
        claim try_claim(const std::string& key, bool allow_idle, connections& closing);
        bool  has_room(const host_connections& host) const noexcept;
        bool  evict_idle(const std::string& except_key, connections& closing);
        void  forget(const std::string& key, std::unique_ptr<connection>&& conn, connections& closing);
        std::optional<io::clock::time_point> close_expired(connections& closing);

        // freed tells waiters that a connection may be free.
        void freed() noexcept;

        io::scheduler*       scheduler;
        client_options       config;
        http2_client_options http2_config;

        mutable std::mutex                                mu;
        std::unordered_map<std::string, host_connections> hosts;
        std::size_t                                       open    = 0;
        bool                                              reaping = false;

        // Bumped whenever a connection is released, for wait_on().
        std::atomic<std::uint64_t> released = 0;

#ifdef NET_HAS_TLS
        // tls_context returns the TLS context for https connections, made (once) the first time one is needed: e.g.
        // loading the system's trust store isn't worth it for a client that never uses https.
        std::expected<tls::client_context, std::error_code> tls_context();

        std::once_flag                                      tls_made;
        std::expected<tls::client_context, std::error_code> tls_ctx{std::unexpect}; // set once, by tls_context()
#endif
    };

}

}
