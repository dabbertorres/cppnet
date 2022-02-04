#pragma once

#include <string_view>

#include "socket.hpp"

namespace net
{

class udp_socket : public socket
{
public:
    udp_socket(std::string_view port, addr_protocol proto = addr_protocol::not_care);
    udp_socket(std::string_view host,
               std::string_view port,
               addr_protocol    proto = addr_protocol::not_care);
    virtual ~udp_socket() = default;

    io_result read(uint8_t* data, size_t length) noexcept override;
    io_result write(const uint8_t* data, size_t length) noexcept override;
};

}
