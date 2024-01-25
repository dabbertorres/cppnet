#pragma once

#include <string_view>

#include "io/scheduler.hpp"

#include "ip_addr.hpp"
#include "socket.hpp"

namespace net
{

class udp_socket : public socket
{
public:
    udp_socket(io::scheduler* scheduler, int fd) noexcept;
    udp_socket(io::scheduler* scheduler, std::string_view port, protocol proto = protocol::not_care);
    udp_socket(io::scheduler*   scheduler,
               std::string_view host,
               std::string_view port,
               protocol         proto = protocol::not_care);

private:
    static int open(std::string_view host, std::string_view port, protocol proto);
};

}
