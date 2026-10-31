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

    // How long a request with "Expect: 100-continue" (which the caller sets: the client never adds it) and a body
    // waits, once its head is sent, for the server's first response before sending its body anyway: a 100 (Continue)
    // sends it at once; another interim response is passed on, and the wait goes on; a final response first doesn't
    // send it unless the connection's to be reused (over HTTP/1.1, whose framing needs it) or it's a 2xx (over HTTP/2,
    // once the wait's over), and a 417 (Expectation Failed) then has the request sent again, at once, without the
    // expectation. 0 sends the body at once, as for a request without it. Over HTTP/1.1 and HTTP/2: see
    // client::send().
    std::chrono::milliseconds expect_continue_timeout = std::chrono::seconds{1};

    // How much an HTTP/1.x response's reader buffers (4 KiB, as Go's; at least 1 KiB, or the constructor throws
    // std::invalid_argument): allocated for each request, and kept until its response's body has been read. A
    // response head line longer than it is read into a string apart, and a body read in pieces smaller than it takes a
    // read of the connection (a syscall) per buffer's worth, where reads larger than it go past it. So larger costs
    // memory per request in flight, and saves reads for long heads and bodies read in small pieces: over loopback, a
    // response with four 1.5 KiB Set-Cookie lines took 70, 62 and 60 us with 1, 4 and 16 KiB, and a chunked download
    // read 256 bytes at a time went at 430, 710 and 840 MiB/s; small responses took as long with each.
    std::size_t read_buffer_size = std::size_t{4} * 1'024;

    // How much of a request's body is read from it before it's written to the connection (HTTP/1.x) or its stream
    // (HTTP/2), each time (16 KiB; at least 1 KiB, or the constructor throws std::invalid_argument): allocated for each
    // request with a body, while it's sent. So larger costs memory per upload in flight, and saves reads of the body
    // and writes (syscalls, over HTTP/1.x).
    std::size_t write_buffer_size = std::size_t{16} * 1'024;

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

    // Whether a request to an https origin whose host has no HTTP/2 connection with a stream free may take a stream of
    // another host's (connection coalescing), rather than connecting: one whose server has said, with an ORIGIN frame
    // (RFC 8336), that it's authoritative for the request's origin, and whose certificate is valid for its host,
    // checked as a new connection's handshake would check it (once per host and connection). Never over cleartext, nor
    // with tls.insecure_skip_verification (nothing then says whose a server is); and only by ORIGIN, never by
    // addresses. Off by default: such a request's host isn't resolved, so whoever holds a certificate valid for its
    // name (mis-issued, or with a stolen key) could take its requests without being on the way to its server, by
    // listing its origin (RFC 8336, section 4: the client checks neither Certificate Transparency nor revocation); and
    // a client that sends requests others choose (e.g. webhooks) alongside its own credentialed ones would let such a
    // server have both. A 421 (Misdirected Request) to a request sent so removes its origin from the connection's set,
    // and has it sent again on a connection of its own host's (see client::send()).
    bool http2_coalescing = false;

    // The TCP_NOTSENT_LOWAT set on each HTTP/2 connection's socket (over TLS, the TCP socket's; not on HTTP/1.x's,
    // whose requests aren't reordered), as the server's (server_config::http2_notsent_lowat): the most the kernel holds
    // written but not yet sent, before the connection's writer, which waits for the socket to take more before each
    // round of frames, frames more. So request bodies sent at once go by their requests' priorities (their Priority
    // fields, see http/priority.hpp) up to the last moment, where otherwise the kernel's send buffer would hold what
    // was framed before a more urgent body came. What's in flight isn't limited by it. 0 sets none, and the writer
    // then writes as the socket takes it.
    std::size_t http2_notsent_lowat = std::size_t{16} * 1'024;

    // Internal (for tests): what an HTTP/2 connection's writer waits with for its socket to take more, instead of the
    // socket (only with http2_notsent_lowat set), if set: e.g. to hold the connection "unwritable".
    detail::writable_function http2_writable{};

    // How much an HTTP/2 connection's reader reads at once (32 KiB; at least 1 KiB, or the constructor throws
    // std::invalid_argument), as the server's (server_config::http2_read_buffer_size).
    std::size_t http2_read_buffer_size = std::size_t{32} * 1'024;

    // How much of a response's body an HTTP/2 server may send ahead of what's been read of it
    // (SETTINGS_INITIAL_WINDOW_SIZE; from 65535 to 2^31-1, or the constructor throws std::invalid_argument): a
    // stream's download goes at most this much a round trip (64 KiB is about 640 KiB/s over a 100 ms round trip). The
    // connection's window is 1 GiB, as Go's, so streams read one at a time don't wait for each other. What a response
    // body that isn't read can make the client hold is this much (in 16 KiB chunks, freed as they're read).
    std::uint32_t http2_stream_window = http2::default_window;

#ifdef NET_HAS_TLS
    // For https:// URLs: which certificate authorities to trust (the system's, by default), or not to verify servers'
    // certificates, for testing; and a certificate (and its key) for servers that ask for one (mutual TLS), the
    // client's for every host (the connections' keys don't change). Its ALPN protocols are ignored: the client offers
    // h2 (if http2 is on) and http/1.1.
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
            std::string                              key; // the connection's host's (see make_key())
            bool reused = false;                          // it was idle in the pool, rather than newly connected

            // it's a stream of another host's HTTP/2 connection (see client_options::http2_coalescing).
            bool coalesced = false;
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
        //
        // With client_options::http2_coalescing (and coalesce), an https request whose host has neither an HTTP/2
        // connection with a stream free nor an idle one (if allow_idle) first looks for a stream of another host's
        // HTTP/2 connection that's authoritative for its origin (see http2_client_connection::authoritative_for()), and
        // reserves it if there's one: the lease is coalesced, and its key is the connection's host's (it stays counted
        // there). It looks again after each wait.
        //
        // With http1_only (for a request the server asked to have over HTTP/1.1: HTTP_1_1_REQUIRED), it returns an
        // HTTP/1.x connection: an idle one to host (if allow_idle), or a new one, which over TLS offers only http/1.1
        // with ALPN, and in cleartext doesn't speak HTTP/2 (even with prior knowledge). It's counted, kept and reused
        // as any HTTP/1.x connection, and says nothing of whether the host speaks HTTP/2: nor do requests to the host
        // wait for it as for an HTTP/2 connection being made. At max_connections_per_host, it closes an idle HTTP/2
        // connection (without streams) of the host's to make room.
        coro::task<std::expected<lease, std::error_code>> acquire(std::string                    scheme,
                                                                  std::string                    host,
                                                                  std::string                    port,
                                                                  bool                           allow_idle,
                                                                  const http2_client_connection* skip       = nullptr,
                                                                  std::stop_token                stop       = {},
                                                                  bool                           coalesce   = true,
                                                                  bool                           http1_only = false);

        // release returns a leased connection: kept idle if it's reusable (it's at the start of a response, with
        // nothing unread), and closed otherwise.
        void release(lease&& leased, bool reusable) noexcept;

        // close_idle closes every idle connection.
        void close_idle() noexcept;

        [[nodiscard]] connection_stats stats() const;
        // stats are the connections to host and port, over any scheme.
        [[nodiscard]] connection_stats stats(const std::string& host, const std::string& port) const;

        // connect_apart connects a connection of the caller's own to host, for a protocol that switches from HTTP/1.1
        // (e.g. WebSocket), as the pool would connect one for scheme ("http" or "https"): resolving the host and
        // connecting within connect_timeout (with attempt_delay and the resolver), then for https the TLS handshake,
        // within tls_handshake_timeout. But it's never the pool's (counted in its limits, or kept), and over TLS it
        // offers only http/1.1 with ALPN, so the server can't choose HTTP/2. It fails as acquire() does (e.g. with
        // std::errc::protocol_not_supported for another scheme), and stop ends it, failing with
        // std::errc::operation_canceled.
        coro::task<std::expected<std::unique_ptr<connection>, std::error_code>>
        connect_apart(const std::string& scheme, std::string host, std::string port, std::stop_token stop);

        // reserve_tunnel reserves a stream for an extended CONNECT (RFC 8441: e.g. a WebSocket's) on one of host's own
        // HTTP/2 connections (never another host's, as coalescing would have it) whose server has said it allows them,
        // and that has a stream free (see http2_client_connection::reserve()), returning it; or none, if there's no
        // such connection: it never connects, nor waits for a stream.
        std::shared_ptr<http2_client_connection>
        reserve_tunnel(const std::string& scheme, const std::string& host, const std::string& port);

        [[nodiscard]] const client_options& options() const noexcept { return config; }

        [[nodiscard]] io::scheduler* get_scheduler() const noexcept { return scheduler; }

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

        // coalescing_connection is an HTTP/2 connection whose Origin Set has been initialized (see
        // http2_client_connection::origins()), and its host's key.
        struct coalescing_connection
        {
            std::shared_ptr<http2_client_connection> conn;
            std::string                              key;
        };

        static std::string make_key(const std::string& scheme, const std::string& host, const std::string& port);

        // connect connects to host, within deadline, and for https, runs a TLS handshake (offering only http/1.1 with
        // ALPN if http1_only, otherwise as tls_context()'s): either ended by stop.
        coro::task<std::expected<std::unique_ptr<connection>, std::error_code>> connect(bool                  tls,
                                                                                        std::string           host,
                                                                                        std::string           port,
                                                                                        io::clock::time_point deadline,
                                                                                        std::stop_token       stop,
                                                                                        bool http1_only = false);

        // wants_http2 is whether a new connection is to speak HTTP/2: it agreed on h2 (or it's cleartext, with prior
        // knowledge).
        [[nodiscard]] bool wants_http2(const connection& conn) const noexcept;

        // start_http2 makes a connection an HTTP/2 connection the host's requests share, returning nullptr if the
        // scheduler's stopping. origin is its own (see origin_text()), over TLS: none in cleartext.
        std::shared_ptr<http2_client_connection>
        start_http2(const std::string& key, std::unique_ptr<connection> conn, std::string origin);

        // available_http2 is one of the host's HTTP/2 connections (other than skip) with a stream free, which it
        // reserves for the request, if there is one (holding mu).
        std::shared_ptr<http2_client_connection> available_http2(const std::string&             key,
                                                                 const http2_client_connection* skip);

        // take_coalesced reserves a stream of one of candidates (another host's connections, from try_claim()) that's
        // authoritative for origin (an https origin, see origin_text()), and has one free, if there is one, not holding
        // mu: the first time a connection's asked for a host, its certificate's verified, on this thread.
        std::optional<lease> take_coalesced(const std::vector<coalescing_connection>& candidates,
                                            const std::string&                        origin);

        // coalescing_candidates fills out with the coalescing connections a request to the host may take, returning
        // whether there are any: other hosts', but skip, unless the host has an idle connection (if allow_idle).
        // Holding mu.
        bool coalescing_candidates(const std::string&                  key,
                                   bool                                allow_idle,
                                   const http2_client_connection*      skip,
                                   std::vector<coalescing_connection>& out) const;

        // uncoalesce takes a connection off coalescing, if it's there (holding mu).
        void uncoalesce(const http2_client_connection* conn) noexcept;

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
            bool                 looked  = false; // at other hosts' connections, which wouldn't take it
        };

        // claim_any is try_claim() (holding mu, reading released into seen first, for waiting), then, if that gave
        // candidates (with coalesce), take_coalesced() for them, without mu: looked, if none of them took it.
        claim claim_any(const std::string&             key,
                        const std::string&             host,
                        const std::string&             port,
                        bool                           tls,
                        bool                           allow_idle,
                        const http2_client_connection* skip,
                        bool                           coalesce,
                        bool                           http1_only,
                        connections&                   closing,
                        std::uint64_t&                 seen);

        // These must be called while holding mu. Connections to close are moved to closing, to close after unlocking.
        // try_claim, given candidates, fills it with the connections a request may be coalesced onto, and returns
        // neither a lease nor room to connect, if the host has no HTTP/2 connection with a stream free, nor an idle one
        // (if allow_idle), and there are any: for take_coalesced() to look through, without mu. With http1_only, it
        // looks only for an idle HTTP/1.x connection, or room (see acquire()).
        claim try_claim(const std::string&                  key,
                        bool                                tls,
                        bool                                allow_idle,
                        const http2_client_connection*      skip,
                        bool                                http1_only,
                        connections&                        closing,
                        std::vector<coalescing_connection>* candidates);
        bool  has_room(const host_connections& host) const noexcept;
        bool  evict_idle(const std::string& except_key, connections& closing);
        bool  evict_own_http2(const std::string& key, connections& closing); // an idle HTTP/2 connection of the host's
        void  forget(const std::string& key, std::unique_ptr<connection>&& conn, connections& closing);
        void  connected(const std::string& key) noexcept; // a connection being made is, or failed

        // not_connected counts out a connection that was being made for a request, but isn't (taking mu).
        void                                 not_connected(const std::string& key, bool http1_only) noexcept;
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

        // the HTTP/2 connections whose Origin Sets have been initialized, with client_options::http2_coalescing: added
        // as each tells of it (http2_client_connection::callbacks::origins_known), taken off as each ends or is
        // evicted. They're also their own hosts' (in hosts), so they're bounded by the connections.
        std::vector<coalescing_connection> coalescing;

        // Bumped whenever a connection is released, for wait_on(); and how many requests wait on it.
        std::atomic<std::uint64_t> released = 0;
        std::atomic<std::size_t>   waiting  = 0;

#ifdef NET_HAS_TLS
        // tls_context returns the TLS context for https connections, made (once) the first time one is needed: e.g.
        // loading the system's trust store isn't worth it for a client that never uses https.
        std::expected<tls::client_context, std::error_code> tls_context();

        std::once_flag                                      tls_made;
        std::expected<tls::client_context, std::error_code> tls_ctx{std::unexpect}; // set once, by tls_context()

        // http1_context returns the TLS context for connect_apart()'s connections, which offers only http/1.1 with
        // ALPN: tls_context()'s, if that's all it offers (with http2 off), or one of its own, made (once) the first
        // time one is needed.
        std::expected<tls::client_context, std::error_code> http1_context();

        std::once_flag                                      http1_made;
        std::expected<tls::client_context, std::error_code> http1_ctx{std::unexpect}; // set once, by http1_context()
#endif
    };

}

}
