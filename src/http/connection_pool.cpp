#include "http/connection_pool.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

#include <sys/socket.h>
#include <sys/types.h>

#include "coro/task.hpp"
#include "io/scheduler.hpp"
#include "tcp.hpp"

namespace
{

// still_open checks that an idle connection hasn't been closed (or sent something unasked for) by the server, which
// is common: servers close idle connections after their own timeouts.
bool still_open(const net::tcp_socket& conn) noexcept
{
    char byte = 0;
    auto n    = ::recv(conn.native_handle(), &byte, 1, MSG_PEEK | MSG_DONTWAIT);
    return n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK);
}

}

namespace net::http::detail
{

connection_pool::connection_pool(io::scheduler* scheduler, const client_options& options)
    : scheduler{scheduler}
    , config{options}
{}

connection_pool::~connection_pool() = default;

std::string connection_pool::make_key(const std::string& host, const std::string& port)
{
    std::string key = host;
    std::ranges::transform(key, key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return key + " " + port;
}

coro::task<std::expected<connection_pool::lease, std::error_condition>>
connection_pool::acquire(std::string host, std::string port, bool allow_idle)
{
    const auto key      = make_key(host, port);
    const auto deadline = io::clock::now() + config.connect_timeout;

    // NOTE: declared before the lock, so the connections are closed after unlocking.
    connections closing;

    while (true)
    {
        std::uint64_t seen = 0;

        {
            std::lock_guard lock{mu};

            // the most recently used first, as the least likely to have been closed by the server.
            // NOTE: forget() erases the host's entry when it closes its last connection, so it's looked up each time.
            for (auto it = hosts.find(key); allow_idle && it != hosts.end() && !it->second.idle.empty();
                 it      = hosts.find(key))
            {
                auto conn = std::move(it->second.idle.back().conn);
                it->second.idle.pop_back();

                if (still_open(*conn)) co_return lease{.conn = std::move(conn), .key = key, .reused = true};

                forget(key, std::move(conn), closing);
            }

            auto& host_conns = hosts[key];

            // at the limit altogether, but not for this host: make room by closing another host's idle connection.
            const bool room = has_room(host_conns)
                           || ((config.max_connections_per_host == 0
                                || host_conns.open < config.max_connections_per_host)
                               && evict_idle(key, closing));

            if (room)
            {
                ++host_conns.open;
                ++open;
                break;
            }

            seen = released.load(std::memory_order::acquire);

            // nothing's open to this host, so don't keep an entry for it while waiting.
            if (host_conns.open == 0) hosts.erase(key);
        }

        closing.clear();

        auto err = co_await scheduler->wait_on(released, seen, deadline);
        if (err) co_return std::unexpected(err);
    }

    closing.clear();

    // connect within what's left of the timeout.
    auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - io::clock::now());
    auto conn      = co_await tcp_socket::connect(scheduler,
                                             host,
                                             port,
                                             {.timeout = std::max(remaining, std::chrono::milliseconds{1}),
                                                   .keepalive = config.tcp_keepalive});

    if (!conn.has_value())
    {
        {
            std::lock_guard lock{mu};
            forget(key, nullptr, closing);
        }

        freed();
        co_return std::unexpected(conn.error());
    }

    co_return lease{.conn = std::make_unique<tcp_socket>(std::move(*conn)), .key = key, .reused = false};
}

void connection_pool::release(lease&& leased, bool reusable) noexcept
{
    connections closing;
    bool        start_reaping = false;

    try
    {
        std::lock_guard lock{mu};

        if (reusable && config.keep_alive && config.idle_timeout > config.idle_timeout.zero())
        {
            hosts[leased.key].idle.push_back(
                {.conn = std::move(leased.conn), .expires = io::clock::now() + config.idle_timeout});

            start_reaping = !std::exchange(reaping, true);
        }
        else
        {
            forget(leased.key, std::move(leased.conn), closing);
        }
    }
    catch (const std::bad_alloc&)
    {
        // the connection is closed, along with leased (but its place in the pool isn't freed: there's no memory).
    }

    freed();

    if (start_reaping && !scheduler->spawn(reap(weak_from_this(), scheduler)))
    {
        // the scheduler is stopping.
        std::lock_guard lock{mu};
        reaping = false;
    }
}

void connection_pool::close_idle() noexcept
{
    connections closing;

    {
        std::lock_guard lock{mu};

        for (auto it = hosts.begin(); it != hosts.end();)
        {
            auto& conns = it->second;
            conns.open -= conns.idle.size();
            open -= conns.idle.size();

            for (auto& idle : conns.idle) closing.push_back(std::move(idle.conn));
            conns.idle.clear();

            it = conns.open == 0 ? hosts.erase(it) : std::next(it);
        }
    }

    freed();
}

connection_stats connection_pool::stats() const
{
    std::lock_guard lock{mu};

    connection_stats out{.open = open};
    for (const auto& [_, conns] : hosts) out.idle += conns.idle.size();
    return out;
}

connection_stats connection_pool::stats(const std::string& host, const std::string& port) const
{
    std::lock_guard lock{mu};

    auto it = hosts.find(make_key(host, port));
    if (it == hosts.end()) return {};
    return {.open = it->second.open, .idle = it->second.idle.size()};
}

coro::task<> connection_pool::reap(std::weak_ptr<connection_pool> weak, io::scheduler* scheduler)
{
    while (true)
    {
        std::optional<io::clock::time_point> next;

        {
            // NOTE: not kept while sleeping: the pool may go away in the meantime.
            auto pool = weak.lock();
            if (!pool) co_return;

            connections closing;
            {
                std::lock_guard lock{pool->mu};
                next = pool->close_expired(closing);
            }

            if (!closing.empty()) pool->freed();
        }

        if (!next) co_return;

        if (auto err = co_await scheduler->sleep_until(*next); err)
        {
            // the scheduler is stopping.
            if (auto pool = weak.lock())
            {
                std::lock_guard lock{pool->mu};
                pool->reaping = false;
            }

            co_return;
        }
    }
}

bool connection_pool::has_room(const host_connections& host) const noexcept
{
    return (config.max_connections_per_host == 0 || host.open < config.max_connections_per_host)
        && (config.max_connections == 0 || open < config.max_connections);
}

bool connection_pool::evict_idle(const std::string& except_key, connections& closing)
{
    // the idle connection that's been idle longest, of any other host.
    auto oldest = hosts.end();
    for (auto it = hosts.begin(); it != hosts.end(); ++it)
    {
        if (it->first == except_key || it->second.idle.empty()) continue;
        if (oldest == hosts.end() || it->second.idle.front().expires < oldest->second.idle.front().expires) oldest = it;
    }

    if (oldest == hosts.end()) return false;

    auto conn = std::move(oldest->second.idle.front().conn);
    oldest->second.idle.pop_front();
    forget(oldest->first, std::move(conn), closing);
    return true;
}

void connection_pool::forget(const std::string& key, std::unique_ptr<tcp_socket>&& conn, connections& closing)
{
    if (conn != nullptr) closing.push_back(std::move(conn));

    --open;

    auto it = hosts.find(key);
    if (it != hosts.end() && --it->second.open == 0) hosts.erase(it);
}

std::optional<io::clock::time_point> connection_pool::close_expired(connections& closing)
{
    const auto                           now = io::clock::now();
    std::optional<io::clock::time_point> next;

    for (auto it = hosts.begin(); it != hosts.end();)
    {
        auto& idle = it->second.idle;

        // oldest first, as they all have the same timeout.
        while (!idle.empty() && idle.front().expires <= now)
        {
            closing.push_back(std::move(idle.front().conn));
            idle.pop_front();
            --it->second.open;
            --open;
        }

        if (!idle.empty() && (!next || idle.front().expires < *next)) next = idle.front().expires;

        it = it->second.open == 0 ? hosts.erase(it) : std::next(it);
    }

    if (!next) reaping = false;
    return next;
}

void connection_pool::freed() noexcept
{
    released.fetch_add(1, std::memory_order::release);
    scheduler->notify_all(released);
}

}
