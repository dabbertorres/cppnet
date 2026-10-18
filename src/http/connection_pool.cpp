#include "http/connection_pool.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <compare>
#include <cstdint>
#include <expected>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <ratio>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <sys/socket.h>

#include "coro/task.hpp"
#include "http/http2.hpp"
#include "http/http2_client.hpp"
#include "http/http2_session.hpp"
#include "io/scheduler.hpp"
#include "tcp.hpp"

namespace
{

// still_open checks that an idle connection hasn't been closed (or sent something unasked for) by the server, which
// is common: servers close idle connections after their own timeouts.
bool still_open(const net::http::detail::connection& conn) noexcept
{
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
#ifdef NET_HAS_TLS
    , config{std::move(options)}
#else
    // NOTE: trivially copyable
    , config{options}
#endif
    , http2_config{
          .response_header_timeout = config.response_header_timeout,
          .io_timeout              = config.io_timeout,
          .idle_timeout            = config.keep_alive ? config.idle_timeout : std::chrono::milliseconds{1},
      }
{
    // a response's head may be as large as HTTP/1.1's (1 MiB), plus 32 bytes a field, for 100 of them.
    http2_config.session.local.max_header_list_size = (std::uint32_t{1} << 20) + (32 * 100);
}

connection_pool::~connection_pool()
{
    // the HTTP/2 connections end (their tasks hold them until they have).
    std::vector<std::shared_ptr<http2_client_connection>> shared;
    {
        std::scoped_lock lock{mu};
        for (auto& [_, conns] : hosts) shared.insert(shared.end(), conns.http2.begin(), conns.http2.end());
    }
    for (const auto& c : shared) c->close();
}

std::string connection_pool::make_key(const std::string& scheme, const std::string& host, const std::string& port)
{
    std::string key = scheme + "://" + host;
    std::ranges::transform(key, key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return key + " " + port;
}

coro::task<std::expected<connection_pool::lease, std::error_code>>
connection_pool::acquire(std::string scheme, std::string host, std::string port, bool allow_idle)
{
#ifdef NET_HAS_TLS
    const bool tls = scheme == "https";
    if (!tls && scheme != "http") co_return std::unexpected(std::make_error_code(std::errc::protocol_not_supported));
#else
    constexpr bool tls = false;
    if (scheme != "http") co_return std::unexpected(std::make_error_code(std::errc::protocol_not_supported));
#endif

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
            found = try_claim(key, allow_idle, closing);
        }

        if (found.leased.has_value()) co_return std::move(*found.leased);
        if (found.connect) break;

        closing.clear();

        auto err = co_await scheduler->wait_on(released, seen, deadline);
        if (err) co_return std::unexpected(err);
    }

    closing.clear();

    auto conn = co_await connect(tls, std::move(host), std::move(port), deadline);
    if (!conn.has_value())
    {
        {
            std::scoped_lock lock{mu};
            forget(key, nullptr, closing);
        }

        freed();
        co_return std::unexpected(conn.error());
    }

    // a connection that speaks HTTP/2 is shared by the host's requests, from now on.
    if (wants_http2(**conn))
    {
        auto shared = start_http2(key, std::move(*conn));
        if (shared == nullptr) co_return std::unexpected(std::make_error_code(std::errc::operation_canceled));
        co_return lease{.conn = nullptr, .http2 = std::move(shared), .key = key, .reused = false};
    }

    co_return lease{.conn = std::move(*conn), .http2 = nullptr, .key = key, .reused = false};
}

connection_pool::claim connection_pool::try_claim(const std::string& key, bool allow_idle, connections& closing)
{
    // an HTTP/2 connection to the host with a stream free; or if all of them are busy, waiting for one, if
    // strict_max_concurrent_streams says to.
    if (auto shared = available_http2(key))
        return {
            .leased = lease{.conn = nullptr, .http2 = std::move(shared), .key = key, .reused = true}
        };

    const auto found = hosts.find(key);
    if (config.strict_max_concurrent_streams && found != hosts.end() && !found->second.http2.empty()) return {};

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

std::shared_ptr<http2_client_connection> connection_pool::start_http2(const std::string&          key,
                                                                      std::unique_ptr<connection> conn)
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
                if (auto it = pool->hosts.find(key); it != pool->hosts.end())
                {
                    std::erase_if(it->second.http2, [&](const auto& c) { return c.get() == &closed; });
                    pool->forget(key, nullptr, closing);
                }
            }
            pool->freed();
        },
        .freed =
            [weak]
        {
            if (auto pool = weak.lock()) pool->freed();
        },
    };

    auto shared = std::make_shared<http2_client_connection>(scheduler, std::move(conn), http2_config, std::move(on));

    // a stream for the request that made it, before others can take them.
    (void)shared->reserve();

    // NOTE: registered before it's started, as it may end (and unregister itself) at once.
    {
        std::scoped_lock lock{mu};
        hosts[key].http2.push_back(shared);
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

    return shared;
}

std::shared_ptr<http2_client_connection> connection_pool::available_http2(const std::string& key)
{
    const auto it = hosts.find(key);
    if (it == hosts.end()) return nullptr;

    for (const auto& c : it->second.http2)
    {
        if (c->reserve()) return c;
    }
    return nullptr;
}

coro::task<std::expected<std::unique_ptr<connection>, std::error_code>>
connection_pool::connect(bool tls, std::string host, std::string port, io::clock::time_point deadline)
{
    // connect within what's left of the timeout.
    auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - io::clock::now());
    auto conn      = co_await tcp_socket::connect(scheduler,
                                                  host,
                                                  port,
                                                  {
                                                      .timeout   = std::max(remaining, std::chrono::milliseconds{1}),
                                                      .keepalive = config.tcp_keepalive,
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

    auto stream = co_await tls::stream::connect(std::move(*conn), *ctx, host, io::clock::now() + handshake_timeout);
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

            it = conns.open == 0 ? hosts.erase(it) : std::next(it);
        }
    }

    for (const auto& c : idle_http2) c->close();
    freed();
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

void connection_pool::forget(const std::string& key, std::unique_ptr<connection>&& conn, connections& closing)
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
