#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <string_view>
/* #include <vector> */

/* #ifndef _WIN32 */
/* #    include <poll.h> */
/* #else */
/* #    error "TODO Windows I/O Completion Ports" */
/* #endif */

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
    listener(const std::string&        port,
             network                   net,
             protocol                  proto   = protocol::not_care,
             std::chrono::microseconds timeout = 5s)
        : listener("", port, net, proto, timeout)
    {}

    listener(const std::string&        host,
             const std::string&        port,
             network                   net,
             protocol                  proto   = protocol::not_care,
             std::chrono::microseconds timeout = 5s);

    listener(const listener&)            = delete;
    listener& operator=(const listener&) = delete;

    listener(listener&&) noexcept;
    listener& operator=(listener&&) noexcept;

    ~listener() noexcept;

    void                     listen(uint16_t max_backlog);
    [[nodiscard]] tcp_socket accept() const;

private:
    std::atomic_bool is_listening;
    int              main_fd;
    /* std::vector<pollfd> fds; */
};

}
