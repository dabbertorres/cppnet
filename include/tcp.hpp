#pragma once

#include <chrono>
#include <string_view>

#include "ip_addr.hpp"
#include "socket.hpp"

namespace net
{

using namespace std::chrono_literals;

class tcp_socket : public socket
{
public:
    tcp_socket() noexcept;
    tcp_socket(int fd) noexcept;

    tcp_socket(std::string_view          host,
               std::string_view          port,
               protocol                  proto     = protocol::not_care,
               bool                      keepalive = true,
               std::chrono::microseconds timeout   = 5s);

private:
    static int open(std::string_view          host,
                    std::string_view          port,
                    protocol                  proto,
                    bool                      keepalive,
                    std::chrono::microseconds timeout);
};

}
