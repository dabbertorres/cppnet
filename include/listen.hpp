#pragma once

#include <atomic>
#include <cstdint>
#include <expected>
#include <string>
#include <system_error>

#include "coro/task.hpp"
#include "io/io.hpp"
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

// listener accepts connections on a port.
class listener
{
public:
    // make binds a listener to host and port (all of the machine's addresses, if host is empty; port "0" lets the OS
    // choose one, see local_port()), and, for TCP, starts listening, with a queue of up to max_backlog connections
    // waiting to be accepted. It fails with getaddrinfo()'s errors (in gai_category()), or the OS's (e.g.
    // std::errc::address_in_use).
    static std::expected<listener, std::error_code> make(io::scheduler*     scheduler,
                                                         const std::string& host,
                                                         const std::string& port,
                                                         network            net         = network::tcp,
                                                         protocol           proto       = protocol::not_care,
                                                         std::uint16_t      max_backlog = 128);

    // The constructors bind the listener as make() does, but don't start listening (see listen()), and throw
    // std::system_error (with make()'s errors) if they can't.
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
    // std::logic_error if it's already listening, or std::system_error if it can't.
    void listen(std::uint16_t max_backlog);

    // listening is whether listen() has been called (or make() did).
    [[nodiscard]] bool listening() const noexcept { return is_listening.load(std::memory_order::acquire); }

    // accept waits for the next connection. Once the listener is shut down, it fails with
    // std::errc::operation_canceled (as do any accepts already waiting).
    [[nodiscard]] coro::task<std::expected<tcp_socket, std::error_code>> accept() const;

    [[nodiscard]] io::handle native_handle() const noexcept { return main_fd; }

    // local_port is the port the listener is bound to: e.g. the one chosen for it, if it was created with port "0". It
    // throws std::system_error if the OS can't say.
    [[nodiscard]] std::uint16_t local_port() const;

    // shutdown stops accepting connections. The socket is closed when the listener is destroyed.
    void shutdown() noexcept;

private:
    listener(io::scheduler* scheduler, io::handle fd) noexcept;

    // open binds a socket, and registers it with the scheduler, returning it.
    static std::expected<io::handle, std::error_code>
    open(io::scheduler* scheduler, const std::string& host, const std::string& port, network net, protocol proto);

    void close() noexcept;

    io::scheduler*    scheduler;
    std::atomic<bool> is_listening;
    io::handle        main_fd;
};

}
