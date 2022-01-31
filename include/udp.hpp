#pragma once

#include <string_view>

#include "socket.hpp"

namespace net
{

class udp_socket : public socket
{
public:
    udp_socket(std::string_view port, addr_protocol proto = addr_protocol::not_care);
    udp_socket(std::string_view host, std::string_view port, addr_protocol proto = addr_protocol::not_care);
    ~udp_socket() = default;

    size_t read(uint8_t* data, size_t length) noexcept override;
    size_t write(const uint8_t* data, size_t length) noexcept override;
};

}

