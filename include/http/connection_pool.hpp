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
#include "io/scheduler.hpp"
#include "tcp.hpp"

namespace net::http
{

using namespace std::chrono_literals;

struct client_options
{
    // How long to wait for a connection: both to connect, and for one to be free when at a limit.
    std::chrono::milliseconds connect_timeout = 10s;

    // Whether to turn on TCP keepalives (SO_KEEPALIVE), which detect connections to hosts that have gone away.
    bool tcp_keepalive = true;

    // Whether to reuse connections for more than one request (HTTP keep-alive).
    bool keep_alive = true;

    // The most connections open at once (in use, or idle), to each host, and altogether. 0 is no limit. A request waits
    // (up to connect_timeout) for a connection to be free when at a limit.
    std::size_t max_connections_per_host = 0;
    std::size_t max_connections          = 0;

    // How long a connection is kept idle, waiting to be reused, before it's closed.
    std::chrono::milliseconds idle_timeout = 90s;

    // How long to wait for a response's head, once its request has been sent. 0 is no limit.
    std::chrono::milliseconds response_header_timeout = 30s;

    // How long sending a request, or reading a response's body, may go without progress: each read or write of the
    // connection may wait this long for it. Slow but steady transfers aren't limited. 0 is no limit.
    std::chrono::milliseconds io_timeout = 30s;
};

struct connection_stats
{
    std::size_t open = 0; // in use, idle, or connecting
    std::size_t idle = 0;

    bool operator==(const connection_stats&) const = default;
};

namespace detail
{

// connection_pool keeps connections open for reuse, keyed by host and port, within client_options' limits.
//
// It's shared (by a client, its copies, and the responses whose bodies are using connections), and closes its idle
// connections when the last of them goes. Connections must be released (or their leases destroyed), even if they
// aren't reusable, so that their places are free for others.
class connection_pool : public std::enable_shared_from_this<connection_pool>
{
public:
    struct lease
    {
        std::unique_ptr<tcp_socket> conn;
        std::string                 key;
        bool                        reused = false; // it was idle in the pool, rather than newly connected
    };

    connection_pool(io::scheduler* scheduler, const client_options& options);

    connection_pool(const connection_pool&)            = delete;
    connection_pool& operator=(const connection_pool&) = delete;
    connection_pool(connection_pool&&)                 = delete;
    connection_pool& operator=(connection_pool&&)      = delete;

    ~connection_pool();

    // acquire returns an idle connection to host (if allow_idle, and one is still open), or connects a new one if the
    // limits allow, waiting for one to be free if not. It fails with std::errc::timed_out if that takes longer than
    // connect_timeout, or as tcp_socket::connect does.
    coro::task<std::expected<lease, std::error_code>> acquire(std::string host, std::string port, bool allow_idle);

    // release returns a leased connection: kept idle if it's reusable (it's at the start of a response, with nothing
    // unread), and closed otherwise.
    void release(lease&& leased, bool reusable) noexcept;

    // close_idle closes every idle connection.
    void close_idle() noexcept;

    [[nodiscard]] connection_stats stats() const;
    [[nodiscard]] connection_stats stats(const std::string& host, const std::string& port) const;

    [[nodiscard]] const client_options& options() const noexcept { return config; }

private:
    using connections = std::vector<std::unique_ptr<tcp_socket>>;

    struct idle_connection
    {
        std::unique_ptr<tcp_socket> conn;
        io::clock::time_point       expires;
    };

    struct host_connections
    {
        std::size_t                 open = 0;
        std::deque<idle_connection> idle; // oldest first
    };

    static std::string make_key(const std::string& host, const std::string& port);

    // reap closes idle connections as they expire, until there are none.
    static coro::task<> reap(std::weak_ptr<connection_pool> weak, io::scheduler* scheduler);

    // These must be called while holding mu. Connections to close are moved to closing, to close after unlocking.
    bool has_room(const host_connections& host) const noexcept;
    bool evict_idle(const std::string& except_key, connections& closing);
    void forget(const std::string& key, std::unique_ptr<tcp_socket>&& conn, connections& closing);
    std::optional<io::clock::time_point> close_expired(connections& closing);

    // freed tells waiters that a connection may be free.
    void freed() noexcept;

    io::scheduler* scheduler;
    client_options config;

    mutable std::mutex                                mu;
    std::unordered_map<std::string, host_connections> hosts;
    std::size_t                                       open = 0;
    bool                                              reaping = false;

    // Bumped whenever a connection is released, for wait_on().
    std::atomic<std::uint64_t> released = 0;
};

}

}
