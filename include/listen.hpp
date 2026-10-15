#pragma once

#include <atomic>
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

enum class network : std::uint8_t
{
    tcp,
    udp,
};

class listener
{
public:
    listener(io::scheduler* scheduler, const std::string& port, network net, protocol proto = protocol::not_care)
        : listener{scheduler, "", port, net, proto}
    {}

    listener(io::scheduler*     scheduler,
             const std::string& host,
             const std::string& port,
             network            net,
             protocol           proto = protocol::not_care);

    listener(const listener&)            = delete;
    listener& operator=(const listener&) = delete;

    listener(listener&& /*other*/) noexcept;
    listener& operator=(listener&& /*other*/) noexcept;

    ~listener() noexcept;

    // listen starts listening, with a queue of up to max_backlog connections waiting to be accepted. It throws
    // std::logic_error if it's already listening.
    void listen(std::uint16_t max_backlog);

    // accept waits for the next connection. Once the listener is shut down, it fails with
    // std::errc::operation_canceled (as do any accepts already waiting).
    [[nodiscard]] coro::task<std::expected<tcp_socket, std::error_condition>> accept() const;

    [[nodiscard]] int native_handle() const noexcept { return main_fd; }

    // local_port is the port the listener is bound to: e.g. the one chosen for it, if it was created with port "0".
    [[nodiscard]] std::uint16_t local_port() const;

    // shutdown stops accepting connections. The socket is closed when the listener is destroyed.
    void shutdown() noexcept;

private:
    void close() noexcept;

    io::scheduler*    scheduler;
    std::atomic<bool> is_listening;
    int               main_fd;
};

}
