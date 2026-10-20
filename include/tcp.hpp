#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <span>
#include <stop_token>
#include <string>
#include <system_error>
#include <utility>

#include <sys/socket.h>

#include "coro/task.hpp"
#include "io/io.hpp"
#include "io/scheduler.hpp"
#include "ip_addr.hpp"
#include "socket.hpp"

namespace net
{

namespace dns
{
    class resolver;
}

namespace detail
{

    // connect_function starts connecting fd to addr, as connect() does on a non-blocking socket: it returns nothing if
    // fd is connected, std::errc::operation_in_progress if it's connecting (and will be writable once it's done), or
    // why it failed.
    using connect_function = std::function<std::error_code(io::handle fd, const sockaddr* addr, socklen_t size)>;

}

class tcp_socket : public socket
{
public:
    struct connect_options
    {
        // Which of the addresses to try: only IPv4's, only IPv6's, or both.
        protocol proto = protocol::not_care;

        // How long to wait for a connection: resolving the host (if it's a name), then connecting to its addresses.
        std::chrono::milliseconds timeout = std::chrono::seconds{10};

        // How long an attempt to connect has before the next starts alongside it (RFC 8305's Connection Attempt
        // Delay; Go's is 300 ms): Happy Eyeballs. The addresses are tried in their order (the resolver's, sorted by
        // RFC 6724), but interleaved by family, the first address's first (e.g. IPv6, IPv4, IPv6, ...; RFC 8305 section
        // 4). Each attempt starts once the one before has had attempt_delay, or at once if it failed; attempts go on
        // together, each until the timeout's end, and the first to connect wins: the rest are stopped, their sockets
        // closed. So an address that doesn't answer (e.g. a broken IPv6 route) costs attempt_delay, not the timeout.
        //
        // 0 tries the addresses one at a time, in their order, each with a share of what's left of the timeout (Go's
        // rule: the time left over the addresses left, but at least 2 seconds, and never past the end), so one that
        // doesn't answer leaves time for the rest.
        std::chrono::milliseconds attempt_delay = std::chrono::milliseconds{250};

        bool keepalive = true;

        // Requesting stop ends connecting (or resolving the host), failing with std::errc::operation_canceled, without
        // trying the addresses left. It only covers connecting: the socket returned doesn't take it (see
        // socket::set_stop_token()).
        std::stop_token stop{};

        // What resolves the host, if it's a name: null is dns::default_resolver(). It must outlive the connect.
        dns::resolver* resolver = nullptr;

        // Internal (for tests): replaces connect() for each attempt, if set. It's called on the connecting task's
        // thread, and mustn't throw.
        detail::connect_function system_connect{};
    };

    explicit tcp_socket(io::scheduler* scheduler) noexcept;
    tcp_socket(io::scheduler* scheduler, io::handle fd);

    // connect resolves host (with options.resolver, see dns::resolver::resolve()), and connects to the first of its
    // addresses that accepts (see connect() for addresses), on port, which must be a number (e.g. "80", not "http").
    // An IP address (e.g. "127.0.0.1" or "::1", or an IPv6 address with a zone, "fe80::1%en0") isn't resolved: it's
    // connected to without a blocking thread (see io::scheduler::run_blocking()). It fails with:
    // - std::errc::invalid_argument if port isn't a number from 0 to 65535, or host isn't a valid name.
    // - the resolver's error if host can't be resolved (compare it with dns::condition: e.g. not_found).
    // - std::errc::address_family_not_supported if the host is an IP address that options.proto rules out.
    // - std::errc::timed_out if resolving the host and connecting took longer than options.timeout altogether.
    // - std::errc::operation_canceled if the scheduler is stopped, or options.stop is.
    // - otherwise, the error of the last attempt to fail (e.g. std::errc::connection_refused).
    static coro::task<std::expected<tcp_socket, std::error_code>>
    connect(io::scheduler* scheduler, std::string host, std::string port, connect_options options);

    static coro::task<std::expected<tcp_socket, std::error_code>>
    connect(io::scheduler* scheduler, std::string host, std::string port)
    {
        return connect(scheduler, std::move(host), std::move(port), connect_options{});
    }

    // connect connects to the first of addresses (those of options.proto's family) that accepts, trying them in order
    // (interleaved by family, see connect_options::attempt_delay), on port, without resolving anything. The addresses
    // are copied before it returns, so they needn't outlive the task. It fails as connect(host, ...) does once that has
    // resolved, and with std::errc::address_family_not_supported if no address is of options.proto's family, or
    // std::errc::invalid_argument if there are none.
    static coro::task<std::expected<tcp_socket, std::error_code>> connect(io::scheduler*           scheduler,
                                                                          std::span<const ip_addr> addresses,
                                                                          std::uint16_t            port,
                                                                          const connect_options&   options);

    static coro::task<std::expected<tcp_socket, std::error_code>>
    connect(io::scheduler* scheduler, std::span<const ip_addr> addresses, std::uint16_t port)
    {
        return connect(scheduler, addresses, port, connect_options{});
    }

    // set_nodelay turns Nagle's algorithm off (TCP_NODELAY), so a small write is sent right away, rather than when what
    // was sent before is acknowledged.
    [[nodiscard]] std::expected<void, std::error_code> set_nodelay(bool nodelay = true) const noexcept;
};

namespace detail
{

    // attempt_deadline is when an attempt to connect to the next of remaining (at least 1) addresses ends, given the
    // deadline for all of them (after now): Go's rule, as connect_options::timeout describes.
    [[nodiscard]] io::clock::time_point
    attempt_deadline(io::clock::time_point now, io::clock::time_point deadline, std::size_t remaining) noexcept;

}

}
