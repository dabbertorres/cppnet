#include "http/connection_pool.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <ratio>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <sys/socket.h>

#include "coro/task.hpp"
#include "http/http2_client.hpp"
#include "http/http2_config.hpp"
#include "http/origin.hpp"
#include "io/scheduler.hpp"
#include "tcp.hpp"
#include "util/defer.hpp"

#ifdef NET_HAS_TLS
#    include <variant>

#    include "tls/tls.hpp"
#endif

namespace
{

// still_open checks that an idle connection hasn't been closed (or sent something unasked for) by the server, which
// is common: servers close idle connections after their own timeouts.
bool still_open(const net::http::detail::connection& conn) noexcept
{
#ifdef NET_HAS_TLS
    // NOTE: what TLS has already read from the socket isn't there to peek at: e.g. the end of the last response's
    //       record, after its body, or close_notify.
    if (const auto* tls = std::get_if<net::tls::stream>(&conn); tls != nullptr && !net::tls::detail::idle_clean(*tls))
        return false;
#endif

    char byte = 0;
    // NOLINTNEXTLINE(clang-analyzer-unix.BlockInCriticalSection): MSG_DONTWAIT, so it doesn't block.
    auto n = ::recv(net::http::detail::native_handle(conn), &byte, 1, MSG_PEEK | MSG_DONTWAIT);
    return n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK);
}

}

namespace net::http::detail
{

connection_pool::connection_pool(io::scheduler* scheduler, client_options options)
    : scheduler{scheduler}
    , config{std::move(options)}
    , http2_config{
          .response_header_timeout = config.response_header_timeout,
          .io_timeout              = config.io_timeout,
          .expect_continue_timeout = config.expect_continue_timeout,
          .idle_timeout            = config.keep_alive ? config.idle_timeout : std::chrono::milliseconds::zero(),
          .notsent_lowat           = config.http2_notsent_lowat,
          .writable                = config.http2_writable,
      }
{
    // a response's head may be as large as HTTP/1.1's (1 MiB), plus 32 bytes a field, for 100 of them.
    http2_config.session.local.max_header_list_size = (std::uint32_t{1} << 20) + (32 * 100);

    // the connection's window holds as many streams' windows as a server's likely to allow (as Go's, 1 GiB), so
    // responses read one at a time don't wait for each other: each stream still has only its own window outstanding.
    http2_config.session.connection_window = std::uint32_t{1} << 30;
}

connection_pool::~connection_pool()
{
    // the HTTP/2 connections end: those without streams at once, the others once their streams are done, so the
    // bodies being read are read to their ends (their tasks, and responses, hold them until they have).
    std::vector<std::shared_ptr<http2_client_connection>> shared;
    {
        std::scoped_lock lock{mu};
        for (auto& [_, conns] : hosts) shared.insert(shared.end(), conns.http2.begin(), conns.http2.end());
    }
    for (const auto& c : shared) c->drain();
}

std::string connection_pool::make_key(const std::string& scheme, const std::string& host, const std::string& port)
{
    std::string key = scheme + "://" + host;
    std::ranges::transform(key, key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return key + " " + port;
}

coro::task<std::expected<connection_pool::lease, std::error_code>>
connection_pool::acquire(std::string                    scheme,
                         std::string                    host,
                         std::string                    port,
                         bool                           allow_idle,
                         const http2_client_connection* skip,
                         std::stop_token                stop)
{
#ifdef NET_HAS_TLS
    const bool tls = scheme == "https";
    if (!tls && scheme != "http") co_return std::unexpected(std::make_error_code(std::errc::protocol_not_supported));
#else
    constexpr bool tls = false;
    if (scheme != "http") co_return std::unexpected(std::make_error_code(std::errc::protocol_not_supported));
#endif

    // NOTE: or it might take a connection (or reserve a stream) it then has to give back.
    if (stop.stop_requested()) co_return std::unexpected(std::make_error_code(std::errc::operation_canceled));

    const auto key      = make_key(scheme, host, port);
    const auto deadline = io::clock::now() + config.connect_timeout;

    // NOTE: declared before the lock, so the connections are closed after unlocking.
    connections closing;

    while (true)
    {
        std::uint64_t seen = 0;
        claim         found;

        {
            std::scoped_lock lock{mu};

            // NOTE: before looking, so that a connection (or stream) freed after this is waited for no longer.
            seen  = released.load(std::memory_order::acquire);
            found = try_claim(key, tls, allow_idle, skip, closing);
        }

        if (found.leased.has_value()) co_return std::move(*found.leased);
        if (found.connect) break;

        closing.clear();

        // counted as waiting before looking again, so that either freed() sees it waiting (and wakes it), or it sees
        // what freed() released (both seq_cst).
        waiting.fetch_add(1, std::memory_order::seq_cst);
        const util::defer waited{[this] noexcept { waiting.fetch_sub(1, std::memory_order::relaxed); }};
        if (released.load(std::memory_order::seq_cst) != seen) continue;

        // NOTE: a token that can't be stopped is as none (nothing's registered).
        auto err = co_await scheduler->wait_on(released, seen, deadline, stop);
        if (err) co_return std::unexpected(err);
    }

    closing.clear();

    // the connection's counted (in open, and as being connected) until it's made: and if this coroutine's destroyed
    // first, counted out.
    bool settled = false;
    // NOTE: forget() and freed() don't throw (nor allocate, given no connection to close).
    util::defer unsettled{[this, &key, &settled] noexcept
                          {
                              if (settled) return;
                              {
                                  connections      none;
                                  std::scoped_lock lock{mu};
                                  connected(key);
                                  forget(key, nullptr, none);
                              }
                              freed();
                          }};

    auto conn = co_await connect(tls, host, port, deadline, std::move(stop));
    if (!conn.has_value()) co_return std::unexpected(conn.error());

    // a connection that speaks HTTP/2 is shared by the host's requests, from now on: over TLS, with its own origin, the
    // first of the Origin Set the server's ORIGIN frames may make it.
    settled = true;
    if (wants_http2(**conn))
    {
        auto shared = start_http2(key, std::move(*conn), tls ? origin_text(host, port) : std::string{});
        if (shared == nullptr) co_return std::unexpected(std::make_error_code(std::errc::operation_canceled));
        co_return lease{.conn = nullptr, .http2 = std::move(shared), .key = key, .reused = false};
    }

    // the host didn't agree on HTTP/2 this time: requests waiting for this connection make their own.
    {
        std::scoped_lock lock{mu};
        connected(key);
        http2_hosts.erase(key);
    }
    freed();

    co_return lease{.conn = std::move(*conn), .http2 = nullptr, .key = key, .reused = false};
}

connection_pool::claim connection_pool::try_claim(
    const std::string& key, bool tls, bool allow_idle, const http2_client_connection* skip, connections& closing)
{
    // an HTTP/2 connection to the host with a stream free.
    if (auto shared = available_http2(key, skip))
        return {
            .leased = lease{.conn = nullptr, .http2 = std::move(shared), .key = key, .reused = true}
        };

    if (const auto found = hosts.find(key); found != hosts.end())
    {
        const auto& conns = found->second;

        // if all of them are busy, waiting for a stream, if strict_max_concurrent_streams says to: of a connection
        // that may still open them (not one that's draining, after GOAWAY).
        const auto may_open = [skip](const auto& c) { return c.get() != skip && c->accepting(); };
        if (config.strict_max_concurrent_streams && std::ranges::any_of(conns.http2, may_open)) return {};

        // a host that speaks HTTP/2 shares a connection: while one's being made, it's waited for, rather than each
        // request making its own.
        if (conns.connecting > 0 && speaks_http2(key, tls)) return {};
    }

    // the most recently used first, as the least likely to have been closed by the server.
    // NOTE: forget() erases the host's entry when it closes its last connection, so it's looked up each time.
    for (auto it = hosts.find(key); allow_idle && it != hosts.end() && !it->second.idle.empty(); it = hosts.find(key))
    {
        auto conn = std::move(it->second.idle.back().conn);
        it->second.idle.pop_back();

        if (still_open(*conn))
            return {
                .leased = lease{.conn = std::move(conn), .http2 = nullptr, .key = key, .reused = true}
            };

        forget(key, std::move(conn), closing);
    }

    auto& host_conns = hosts[key];

    // at the limit altogether, but not for this host: make room by closing another host's idle connection.
    const bool room = has_room(host_conns)
                   || ((config.max_connections_per_host == 0 || host_conns.open < config.max_connections_per_host)
                       && evict_idle(key, closing));

    if (room)
    {
        ++host_conns.open;
        ++host_conns.connecting;
        ++open;
        return {.connect = true};
    }

    // nothing's open to this host, so don't keep an entry for it while waiting.
    if (host_conns.open == 0) hosts.erase(key);
    return {};
}

bool connection_pool::wants_http2(const connection& conn) const noexcept
{
    if (!config.http2) return false;
    return is_tls(conn) ? alpn(conn) == "h2" : config.http2_prior_knowledge;
}

bool connection_pool::speaks_http2(const std::string& key, bool tls) const
{
    if (!config.http2) return false;
    return (!tls && config.http2_prior_knowledge) || http2_hosts.contains(key);
}

void connection_pool::host_gone(const std::string& key) { http2_hosts.erase(key); }

std::shared_ptr<http2_client_connection>
connection_pool::start_http2(const std::string& key, std::unique_ptr<connection> conn, std::string origin)
{
    std::weak_ptr<connection_pool> weak = weak_from_this();

    http2_client_connection::callbacks on{
        .closed =
            [weak, key](http2_client_connection& closed)
        {
            auto pool = weak.lock();
            if (!pool) return;

            connections closing;
            {
                std::scoped_lock lock{pool->mu};

                // NOTE: unless it's been counted out already (evicted).
                const auto it = pool->hosts.find(key);
                if (it != pool->hosts.end()
                    && std::erase_if(it->second.http2, [&](const auto& c) { return c.get() == &closed; }) > 0)
                    pool->forget(key, nullptr, closing);
            }
            pool->freed();
        },
        .freed =
            [weak]
        {
            if (auto pool = weak.lock()) pool->freed();
        },
        // NOTE: not used yet: the pool's coalescing is the HTTP/2 plan's stage 6's twelfth part.
        .origins_known = nullptr,
    };

    // a writer stuck (the server not reading) fails the connection, and so its streams, as the server's
    // write_timeout does: nothing else may notice, e.g. a request whose head is queued behind, without a
    // response_header_timeout.
    if (config.io_timeout > std::chrono::milliseconds::zero()) socket_of(*conn).set_write_timeout(config.io_timeout);

    auto shared = std::make_shared<http2_client_connection>(scheduler,
                                                            std::move(conn),
                                                            http2_config,
                                                            std::move(on),
                                                            std::move(origin));

    // a stream for the request that made it, before others can take them.
    (void)shared->reserve();

    // NOTE: registered before it's started, as it may end (and unregister itself) at once.
    {
        std::scoped_lock lock{mu};
        connected(key);
        hosts[key].http2.push_back(shared);
        http2_hosts.insert(key);
    }

    if (!shared->start())
    {
        connections closing;
        {
            std::scoped_lock lock{mu};
            if (auto it = hosts.find(key); it != hosts.end()) std::erase(it->second.http2, shared);
            forget(key, nullptr, closing);
        }
        freed();
        return nullptr;
    }

    // requests waiting for it (to a host known to speak HTTP/2) may take its streams.
    freed();
    return shared;
}

std::shared_ptr<http2_client_connection> connection_pool::available_http2(const std::string&             key,
                                                                          const http2_client_connection* skip)
{
    const auto it = hosts.find(key);
    if (it == hosts.end()) return nullptr;

    for (const auto& c : it->second.http2)
    {
        if (c.get() != skip && c->reserve()) return c;
    }
    return nullptr;
}

coro::task<std::expected<std::unique_ptr<connection>, std::error_code>> connection_pool::connect(
    bool tls, std::string host, std::string port, io::clock::time_point deadline, std::stop_token stop)
{
    // connect (resolving the host, then connecting to its addresses) within what's left of the timeout: rounded up, so
    // it doesn't give up before the deadline.
    auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - io::clock::now());
    auto conn      = co_await tcp_socket::connect(scheduler,
                                                  host,
                                                  port,
                                                  {
                                                      .timeout       = std::max(remaining, std::chrono::milliseconds{1}),
                                                      .attempt_delay = config.attempt_delay,
                                                      .keepalive     = config.tcp_keepalive,
                                                      .stop          = stop,
                                                      .resolver      = config.resolver,
                                                  });
    if (!conn.has_value()) co_return std::unexpected(conn.error());

    if (!tls) co_return std::make_unique<connection>(std::move(*conn));

#ifdef NET_HAS_TLS
    auto ctx = tls_context();
    if (!ctx.has_value()) co_return std::unexpected(ctx.error());

    // NOTE: 0 is no limit, but the handshake needs a deadline.
    const auto handshake_timeout = config.tls_handshake_timeout > std::chrono::milliseconds::zero()
                                     ? io::clock::duration{config.tls_handshake_timeout}
                                     : io::clock::duration{std::chrono::hours{24} * 365};

    auto stream = co_await tls::stream::connect(std::move(*conn),
                                                *ctx,
                                                host,
                                                io::clock::now() + handshake_timeout,
                                                std::move(stop));
    if (!stream.has_value()) co_return std::unexpected(stream.error());

    co_return std::make_unique<connection>(std::move(*stream));
#else
    co_return std::unexpected(std::make_error_code(std::errc::protocol_not_supported));
#endif
}

#ifdef NET_HAS_TLS
std::expected<tls::client_context, std::error_code> connection_pool::tls_context()
{
    std::call_once(tls_made,
                   [this]
                   {
                       auto options = config.tls;
                       options.alpn = config.http2 ? std::vector<std::string>{"h2", "http/1.1"}
                                                   : std::vector<std::string>{"http/1.1"};
                       tls_ctx      = tls::client_context::make(options);
                   });

    return tls_ctx;
}
#endif

// NOLINTNEXTLINE(cppcoreguidelines-rvalue-reference-param-not-moved): its members are moved from.
void connection_pool::release(lease&& leased, bool reusable) noexcept
{
    connections closing;
    bool        start_reaping = false;

    {
        std::scoped_lock lock{mu};

        if (reusable && config.keep_alive && config.idle_timeout > std::chrono::milliseconds::zero())
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

    freed();

    if (start_reaping && !scheduler->spawn(reap(weak_from_this(), scheduler)))
    {
        // the scheduler is stopping.
        std::scoped_lock lock{mu};
        reaping = false;
    }
}

void connection_pool::close_idle() noexcept
{
    connections                                           closing;
    std::vector<std::shared_ptr<http2_client_connection>> idle_http2;

    {
        std::scoped_lock lock{mu};

        for (auto it = hosts.begin(); it != hosts.end();)
        {
            auto& conns = it->second;

            // HTTP/2 connections without streams are closed too (and unregister themselves once they have).
            for (const auto& c : conns.http2)
            {
                if (c->idle()) idle_http2.push_back(c);
            }

            conns.open -= conns.idle.size();
            open -= conns.idle.size();

            for (auto& idle : conns.idle) closing.push_back(std::move(idle.conn));
            conns.idle.clear();

            if (conns.open != 0)
            {
                ++it;
                continue;
            }

            host_gone(it->first);
            it = hosts.erase(it);
        }
    }

    for (const auto& c : idle_http2) c->close();
    freed();
}

std::size_t connection_pool::known_http2_hosts() const
{
    std::scoped_lock lock{mu};
    return http2_hosts.size();
}

bool connection_pool::knows_http2(const std::string& scheme, const std::string& host, const std::string& port) const
{
    std::scoped_lock lock{mu};
    return http2_hosts.contains(make_key(scheme, host, port));
}

connection_stats connection_pool::stats() const
{
    std::scoped_lock lock{mu};

    connection_stats out{.open = open};
    for (const auto& [_, conns] : hosts) out.idle += idle_count(conns);
    return out;
}

connection_stats connection_pool::stats(const std::string& host, const std::string& port) const
{
    std::scoped_lock lock{mu};

    connection_stats out;
    for (const auto* scheme : {"http", "https"})
    {
        auto it = hosts.find(make_key(scheme, host, port));
        if (it == hosts.end()) continue;

        out.open += it->second.open;
        out.idle += idle_count(it->second);
    }

    return out;
}

std::size_t connection_pool::idle_count(const host_connections& conns)
{
    // an HTTP/2 connection is idle when it has no streams open.
    return conns.idle.size()
         + static_cast<std::size_t>(std::ranges::count_if(conns.http2, [](const auto& c) { return c->idle(); }));
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
                std::scoped_lock lock{pool->mu};
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
                std::scoped_lock lock{pool->mu};
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
    // the connection that's been idle longest, of any other host: an HTTP/1.x one waiting to be reused, or an HTTP/2
    // one without streams (nor any reserved).
    auto                                     oldest = hosts.end();
    std::shared_ptr<http2_client_connection> oldest_http2;
    io::clock::time_point                    oldest_since{};

    const auto consider = [&](auto it, std::shared_ptr<http2_client_connection> http2, io::clock::time_point since)
    {
        if (oldest != hosts.end() && since >= oldest_since) return;
        oldest       = it;
        oldest_http2 = std::move(http2);
        oldest_since = since;
    };

    for (auto it = hosts.begin(); it != hosts.end(); ++it)
    {
        if (it->first == except_key) continue;

        auto& conns = it->second;
        if (!conns.idle.empty()) consider(it, nullptr, conns.idle.front().expires - config.idle_timeout);

        for (const auto& c : conns.http2)
        {
            if (const auto since = c->idle_since(); since.has_value()) consider(it, c, *since);
        }
    }

    if (oldest == hosts.end()) return false;

    if (oldest_http2 != nullptr)
    {
        // counted out now, so there's room at once, though it closes in its own time.
        std::erase(oldest->second.http2, oldest_http2);
        oldest_http2->close();
        forget(oldest->first, nullptr, closing);
        return true;
    }

    auto conn = std::move(oldest->second.idle.front().conn);
    oldest->second.idle.pop_front();
    forget(oldest->first, std::move(conn), closing);
    return true;
}

void connection_pool::forget(const std::string& key, std::unique_ptr<connection>&& conn, connections& closing)
{
    if (conn != nullptr) closing.push_back(std::move(conn));

    --open;

    auto it = hosts.find(key);
    if (it != hosts.end() && --it->second.open == 0)
    {
        host_gone(key);
        hosts.erase(it);
    }
}

void connection_pool::connected(const std::string& key) noexcept
{
    if (auto it = hosts.find(key); it != hosts.end() && it->second.connecting > 0) --it->second.connecting;
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

        if (it->second.open != 0)
        {
            ++it;
            continue;
        }

        host_gone(it->first);
        it = hosts.erase(it);
    }

    if (!next) reaping = false;
    return next;
}

void connection_pool::freed() noexcept
{
    released.fetch_add(1, std::memory_order::seq_cst);

    // e.g. each HTTP/2 stream that ends: the scheduler's waiters (behind its lock) are woken only if there are any.
    if (waiting.load(std::memory_order::seq_cst) > 0) scheduler->notify_all(released);
}

}
