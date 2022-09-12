#pragma once

#include <string_view>

#include "socket.hpp"

namespace net
{

class udp_socket : public socket
{
public:
    udp_socket(std::string_view port, protocol proto = protocol::not_care);
    udp_socket(std::string_view host, std::string_view port, protocol proto = protocol::not_care);
};

}
