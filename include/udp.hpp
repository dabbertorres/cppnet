#pragma once

#include <string_view>

#include "ip_addr.hpp"
#include "socket.hpp"

namespace net
{

class udp_socket : public socket
{
public:
    udp_socket(int fd) noexcept;
    udp_socket(std::string_view port, protocol proto = protocol::not_care);
    udp_socket(std::string_view host, std::string_view port, protocol proto = protocol::not_care);

private:
    static int open(std::string_view host, std::string_view port, protocol proto);
};

}
