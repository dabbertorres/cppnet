#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <stop_token>
#include <string>
#include <system_error>
#include <utility>

#include "coro/task.hpp"
#include "io/io.hpp"
#include "io/scheduler.hpp"
#include "ip_addr.hpp"
#include "socket.hpp"

namespace net
{

class tcp_socket : public socket
{
public:
    struct connect_options
    {
        // Which of the addresses to try: only IPv4's, only IPv6's, or both.
        protocol proto = protocol::not_care;

        // How long to wait for a connection, over all of the addresses. Each attempt gets a share of what's left (Go's
        // rule: the time left over the addresses left, but at least 2 seconds, and never past the end), so one that
        // doesn't answer leaves time for the rest. (Resolving the host isn't included: getaddrinfo() has its own
        // timeouts.)
        std::chrono::milliseconds timeout = std::chrono::seconds{10};

        bool keepalive = true;

        // Requesting stop ends connecting, failing with std::errc::operation_canceled, without trying the addresses
        // left. Resolving a host name can't be interrupted (getaddrinfo() blocks), so stop requested meanwhile is seen
        // once it's done. It only covers connecting: the socket returned doesn't take it (see
        // socket::set_stop_token()).
        std::stop_token stop{};
    };

    explicit tcp_socket(io::scheduler* scheduler) noexcept;
    tcp_socket(io::scheduler* scheduler, io::handle fd);

    // connect resolves host, and connects to the first of its addresses that accepts (see connect() for addresses).
    // An IP address (in its usual form, e.g. "127.0.0.1" or "::1") with a numeric port isn't resolved: it's connected
    // to without a blocking thread (see io::scheduler::run_blocking()). It fails with:
    // - an error in gai_category() if the host can't be resolved.
    // - std::errc::address_family_not_supported if the host is an IP address that options.proto rules out.
    // - std::errc::timed_out if no address accepted in time.
    // - std::errc::operation_canceled if the scheduler is stopped, or options.stop is.
    // - otherwise, the error from the last address tried (e.g. std::errc::connection_refused).
    static coro::task<std::expected<tcp_socket, std::error_code>>
    connect(io::scheduler* scheduler, std::string host, std::string port, connect_options options);

    static coro::task<std::expected<tcp_socket, std::error_code>>
    connect(io::scheduler* scheduler, std::string host, std::string port)
    {
        return connect(scheduler, std::move(host), std::move(port), connect_options{});
    }

    // connect connects to the first of addresses (those of options.proto's family) that accepts, trying them in order,
    // on port, without resolving anything. The addresses are copied before it returns, so they needn't outlive the
    // task. It fails as connect(host, ...) does once that has resolved, and with
    // std::errc::address_family_not_supported if no address is of options.proto's family, or
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
