#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <string>
#include <system_error>

#include "coro/task.hpp"
#include "io/scheduler.hpp"

#include "ip_addr.hpp"
#include "tcp.hpp"

namespace net
{

using namespace std::chrono_literals;

enum class network
{
    tcp,
    udp,
};

class listener
{
public:
    listener(io::scheduler*            scheduler,
             const std::string&        port,
             network                   net,
             protocol                  proto   = protocol::not_care,
             std::chrono::microseconds timeout = 5s)
        : listener{scheduler, "", port, net, proto, timeout}
    {}

    listener(io::scheduler*            scheduler,
             const std::string&        host,
             const std::string&        port,
             network                   net,
             protocol                  proto   = protocol::not_care,
             std::chrono::microseconds timeout = 5s);

    listener(const listener&)            = delete;
    listener& operator=(const listener&) = delete;

    listener(listener&&) noexcept;
    listener& operator=(listener&&) noexcept;

    ~listener() noexcept;

    void listen(std::uint16_t max_backlog);

    // accept waits for the next connection. Once the listener is shut down, it fails with
    // std::errc::operation_canceled (as do any accepts already waiting).
    [[nodiscard]] coro::task<std::expected<tcp_socket, std::error_condition>> accept() const;

    [[nodiscard]] int native_handle() const noexcept { return main_fd; }

    // shutdown stops accepting connections. The socket is closed when the listener is destroyed.
    void shutdown() noexcept;

private:
    void close() noexcept;

    io::scheduler*    scheduler;
    std::atomic<bool> is_listening;
    int               main_fd;
};

}
