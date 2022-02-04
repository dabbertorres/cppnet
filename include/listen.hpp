#pragma once

#include <chrono>
#include <memory>
#include <string_view>

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
             addr_protocol                      proto   = addr_protocol::not_care,
             std::chrono::duration<Rep, Period> timeout = 5s) :
        listener(
            "", port, net, proto, std::chrono::duration_cast<std::chrono::microseconds>(timeout))
    {}

    template<typename Rep, typename Period>
    listener(std::string_view                   host,
             std::string_view                   port,
             network                            net,
             addr_protocol                      proto   = addr_protocol::not_care,
             std::chrono::duration<Rep, Period> timeout = 5s) :
        listener(
            host, port, net, proto, std::chrono::duration_cast<std::chrono::microseconds>(timeout))
    {}

    bool       listen(size_t max_pending) const noexcept;
    tcp_socket accept() const;

private:
    listener(std::string_view          host,
             std::string_view          port,
             network                   net,
             addr_protocol             proto,
             std::chrono::microseconds timeout);

    int fd;
};

}
