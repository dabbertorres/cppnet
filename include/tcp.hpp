#pragma once

#include <chrono>
#include <string_view>

#include "socket.hpp"

namespace net
{

using namespace std::chrono_literals;

class tcp_socket : public socket
{
public:
    tcp_socket(int fd);

    tcp_socket(std::string_view          host,
               std::string_view          port,
               protocol                  proto     = protocol::not_care,
               bool                      keepalive = true,
               std::chrono::microseconds timeout   = 5s);
};

}
