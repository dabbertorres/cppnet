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
#include <stop_token>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "coro/task.hpp"
#include "dns/resolver.hpp"
#include "http/connection.hpp"
#include "http/http2_config.hpp"
#include "io/scheduler.hpp"

#ifdef NET_HAS_TLS
#    include "tls/tls.hpp"
#endif

namespace net::http
{

struct client_options
{
    // How long to wait for a connection: both to connect (resolving the host's name, then connecting to its
    // addresses), and for one to be free when at a limit.
    std::chrono::milliseconds connect_timeout = std::chrono::seconds{10};

    // How long an attempt to connect to one of a host's addresses has before the next starts alongside it (Happy
    // Eyeballs, RFC 8305's Connection Attempt Delay): see tcp_socket::connect_options::attempt_delay. 0 tries them one
    // at a time.
    std::chrono::milliseconds attempt_delay = std::chrono::milliseconds{250};

    // What resolves hosts' names: null is dns::default_resolver(). It must outlive the client (and its copies, and the
    // responses whose bodies are being read).
    dns::resolver* resolver = nullptr;

    // Whether to turn on TCP keepalives (SO_KEEPALIVE), which detect connections to hosts that have gone away.
    bool tcp_keepalive = true;

    // Whether to reuse connections for more than one request (HTTP keep-alive).
    bool keep_alive = true;

    // The most connections open at once (in use, or idle), to each host, and altogether. 0 is no limit. A request waits
    // (up to connect_timeout) for a connection to be free when at a limit.
    std::size_t max_connections_per_host = 0;
    std::size_t max_connections          = 0;

    // How long a connection is kept idle, waiting to be reused, before it's closed: an HTTP/1.x connection between
    // requests, or an HTTP/2 connection without streams. 0 keeps none: a connection closes once its (last) response
    // is done.
    std::chrono::milliseconds idle_timeout = std::chrono::seconds{90};

    // How many redirects (301, 302, 303, 307 and 308, with a Location) a request follows, before failing with
    // client_errc::too_many_redirects: 0 (the default) follows none, so send() returns the redirect's response; Go's 10
    // is a sensible limit. A request may override it (client_request::max_redirects). See client::send().
    std::size_t max_redirects = 0;

    // How long to wait for a response's head, once its request has been sent. 0 is no limit.
    std::chrono::milliseconds response_header_timeout = std::chrono::seconds{30};

    // How long sending a request, or reading a response's body, may go without progress: each read or write of the
    // connection may wait this long for it. Slow but steady transfers aren't limited. 0 is no limit. An HTTP/2
    // connection whose writes stall this long fails, with its streams.
    std::chrono::milliseconds io_timeout = std::chrono::seconds{30};

    // Whether to speak HTTP/2 (each request a stream of a connection that others share) to servers that do: over TLS,
    // it's offered with ALPN, and used if the server agrees. A request's HTTP/1.x version and Connection field mean
    // nothing over HTTP/2: turning this off is how to send HTTP/1.0, or close a connection after a request.
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

    class http2_client_connection;

    // connection_pool keeps connections open for reuse, keyed by scheme, host and port, within client_options' limits.
    //
    // It's shared (by a client, its copies, and the responses whose bodies are using HTTP/1.x connections), and closes
    // its idle connections when the last of them goes; HTTP/2 connections with streams get GOAWAY, and close once
    // their streams are done (their responses' bodies hold them). Connections must be released (or their leases
    // destroyed), even if they aren't reusable, so that their places are free for others. At max_connections, an
    // idle connection to another host (HTTP/1.x, or HTTP/2 without streams) is closed to make room.
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

        // acquire returns an idle connection to host (if allow_idle, and one is still open), or a stream of an HTTP/2
        // connection (other than skip, e.g. one that refused the request), or connects a new one if the limits allow,
        // waiting for one to be free if not, and for https, runs a TLS handshake over it. To a host known to speak
        // HTTP/2 (see speaks_http2), a request waits for a connection being made rather than making its own. It fails
        // with std::errc::timed_out if connecting takes longer than connect_timeout (or the handshake
        // tls_handshake_timeout), as tcp_socket::connect does, or with TLS's errors (e.g. a certificate that couldn't
        // be verified). scheme is "http" or "https" (or with NET_HAS_TLS undefined, only "http").
        //
        // Requesting stop ends it (waiting, connecting, or the handshake), failing with std::errc::operation_canceled;
        // a connection that was being made for it is closed (and counted out). Once it's returned a lease, stop isn't
        // its business: an HTTP/2 lease's stream stays reserved until its send(), which then fails (giving it back).
        coro::task<std::expected<lease, std::error_code>> acquire(std::string                    scheme,
                                                                  std::string                    host,
                                                                  std::string                    port,
                                                                  bool                           allow_idle,
                                                                  const http2_client_connection* skip = nullptr,
                                                                  std::stop_token                stop = {});

        // release returns a leased connection: kept idle if it's reusable (it's at the start of a response, with
        // nothing unread), and closed otherwise.
        void release(lease&& leased, bool reusable) noexcept;

        // close_idle closes every idle connection.
        void close_idle() noexcept;

        [[nodiscard]] connection_stats stats() const;
        // stats are the connections to host and port, over any scheme.
        [[nodiscard]] connection_stats stats(const std::string& host, const std::string& port) const;

        [[nodiscard]] const client_options& options() const noexcept { return config; }

        // For the tests: how many hosts the pool remembers speak HTTP/2, and whether it remembers that one does.
        [[nodiscard]] std::size_t known_http2_hosts() const;
        [[nodiscard]] bool
        knows_http2(const std::string& scheme, const std::string& host, const std::string& port) const;

    private:
        using connections = std::vector<std::unique_ptr<connection>>;

        struct idle_connection
        {
            std::unique_ptr<connection> conn;
            io::clock::time_point       expires;
        };

        struct host_connections
        {
            std::size_t                 open       = 0;
            std::size_t                 connecting = 0; // of open, being connected
            std::deque<idle_connection> idle;           // oldest first

            // NOTE: one evicted is dropped from here at once (and counted out of open), so that its closed callback,
            //       finding it gone, doesn't count it out again.
            std::vector<std::shared_ptr<http2_client_connection>> http2;
        };

        static std::string make_key(const std::string& scheme, const std::string& host, const std::string& port);

        // connect connects to host, within deadline, and for https, runs a TLS handshake: either ended by stop.
        coro::task<std::expected<std::unique_ptr<connection>, std::error_code>>
        connect(bool tls, std::string host, std::string port, io::clock::time_point deadline, std::stop_token stop);

        // wants_http2 is whether a new connection is to speak HTTP/2: it agreed on h2 (or it's cleartext, with prior
        // knowledge).
        [[nodiscard]] bool wants_http2(const connection& conn) const noexcept;

        // start_http2 makes a connection an HTTP/2 connection the host's requests share, returning nullptr if the
        // scheduler's stopping.
        std::shared_ptr<http2_client_connection> start_http2(const std::string& key, std::unique_ptr<connection> conn);

        // available_http2 is one of the host's HTTP/2 connections (other than skip) with a stream free, which it
        // reserves for the request, if there is one (holding mu).
        std::shared_ptr<http2_client_connection> available_http2(const std::string&             key,
                                                                 const http2_client_connection* skip);

        // speaks_http2 is whether host is known to speak HTTP/2 (holding mu): it's cleartext, with prior knowledge, or
        // one of its connections agreed on HTTP/2 and it has had connections since (see http2_hosts).
        [[nodiscard]] bool speaks_http2(const std::string& key, bool tls) const;

        // host_gone forgets what the pool knew of host, whose last connection has gone (holding mu).
        void host_gone(const std::string& key);

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
        claim try_claim(const std::string&             key,
                        bool                           tls,
                        bool                           allow_idle,
                        const http2_client_connection* skip,
                        connections&                   closing);
        bool  has_room(const host_connections& host) const noexcept;
        bool  evict_idle(const std::string& except_key, connections& closing);
        void  forget(const std::string& key, std::unique_ptr<connection>&& conn, connections& closing);
        void  connected(const std::string& key) noexcept; // a connection being made is, or failed
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

        // the hosts one of whose connections agreed on HTTP/2, forgotten once they have none (as Go's pool keeps
        // nothing of a host but its connections): so it's bounded by the connections, and a burst of requests to a host
        // with none connects in parallel, while one to a host with some waits for a connection being made.
        std::unordered_set<std::string> http2_hosts;

        // Bumped whenever a connection is released, for wait_on(); and how many requests wait on it.
        std::atomic<std::uint64_t> released = 0;
        std::atomic<std::size_t>   waiting  = 0;

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
