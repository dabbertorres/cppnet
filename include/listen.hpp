#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

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

    void                     listen(std::uint16_t max_backlog);
    [[nodiscard]] tcp_socket accept() const;

    [[nodiscard]] int native_handle() const noexcept { return main_fd; }

private:
    io::scheduler*   scheduler;
    std::atomic_bool is_listening;
    int              main_fd;
    /* std::vector<pollfd> fds; */
};

}
