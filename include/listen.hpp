#pragma once

#include <atomic>
#include <coroutine>
#include <cstdint>
#include <expected>
#include <optional>
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
    // std::errc::operation_canceled (as do any accepts already waiting), as it does when the scheduler is stopped. It
    // takes a connection that's already waiting without a coroutine (see coro::ready_task), and waits for one through
    // the listener's accept operation (also without one), so only one accept may wait at a time: another that has to
    // wait meanwhile fails with std::errc::device_or_resource_busy, as a socket's second read does. (Accepts that don't
    // have to wait aren't limited.)
    [[nodiscard]] coro::task<std::expected<tcp_socket, std::error_code>> accept();

    [[nodiscard]] io::handle native_handle() const noexcept { return main_fd; }

    // local_port is the port the listener is bound to: e.g. the one chosen for it, if it was created with port "0". It
    // throws std::system_error if the OS can't say.
    [[nodiscard]] std::uint16_t local_port() const;

    // shutdown stops accepting connections. The socket is closed when the listener is destroyed.
    void shutdown() noexcept;

private:
    // accept_operation accepts a connection once the listener's ready, which the scheduler does (see
    // io::scheduler::handle_operation), for accept() to return as a task when its first try would have to wait, rather
    // than a coroutine that waits and tries again in turn. A listener has one, as only one task may wait to accept at a
    // time: claim() fails while one is.
    class accept_operation final
        : public coro::operation<std::expected<tcp_socket, std::error_code>>
        , public io::scheduler::handle_operation
    {
    public:
        explicit accept_operation(const listener& owner) noexcept
            : owner{&owner}
        {}

        // claim readies it to accept, returning false if it's already in use.
        [[nodiscard]] bool claim() noexcept;

        bool                                       start(std::coroutine_handle<> awaiting) noexcept override;
        std::expected<tcp_socket, std::error_code> result() override;
        void                                       cancel() noexcept override;
        bool                                       perform() noexcept override;

    private:
        const listener* owner;

        // accepted is the connection perform() accepted (until result() makes a socket of it), and failed why it
        // couldn't, if it couldn't.
        io::handle      accepted = -1;
        std::error_code failed;
    };

    listener(io::scheduler* scheduler, io::handle fd) noexcept;

    // open binds a socket, and registers it with the scheduler, returning it.
    static std::expected<io::handle, std::error_code>
    open(io::scheduler* scheduler, const std::string& host, const std::string& port, network net, protocol proto);

    void close() noexcept;

    // try_accept accepts a connection, unless it would have to wait (then returning nothing): the connection's handle,
    // or why it couldn't.
    [[nodiscard]] std::optional<std::expected<io::handle, std::error_code>> try_accept() const noexcept;

    io::scheduler*    scheduler;
    std::atomic<bool> is_listening;
    io::handle        main_fd;

    // NOTE: not moved with the listener (each has its own): one mustn't be moved while an accept waits.
    accept_operation accepting{*this};
};

}
