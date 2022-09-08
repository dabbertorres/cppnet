#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <string_view>
/* #include <vector> */

/* #ifndef _WIN32 */
/* #    include <poll.h> */
/* #else */
/* #    error "TODO Windows I/O Completion Ports" */
/* #endif */

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
    template<typename Rep, typename Period>
    listener(std::string_view                   port,
             network                            net,
             protocol                           proto   = protocol::not_care,
             std::chrono::duration<Rep, Period> timeout = 5s) :
        listener(
            "", port, net, proto, std::chrono::duration_cast<std::chrono::microseconds>(timeout))
    {}

    template<typename Rep, typename Period>
    listener(std::string_view                   host,
             std::string_view                   port,
             network                            net,
             protocol                           proto   = protocol::not_care,
             std::chrono::duration<Rep, Period> timeout = 5s) :
        listener(
            host, port, net, proto, std::chrono::duration_cast<std::chrono::microseconds>(timeout))
    {}

    listener(const listener&) = delete;

    ~listener() noexcept;

    void       listen(uint16_t max_backlog) const;
    tcp_socket accept() const;

private:
    listener(std::string_view          host,
             std::string_view          port,
             network                   net,
             protocol                  proto,
             std::chrono::microseconds timeout);

    std::atomic_bool is_listening;
    int              main_fd;
    /* std::vector<pollfd> fds; */
};

}
