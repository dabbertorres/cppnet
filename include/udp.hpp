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

    udp_socket(const udp_socket&)            = delete;
    udp_socket& operator=(const udp_socket&) = delete;

    udp_socket(udp_socket&&) noexcept            = default;
    udp_socket& operator=(udp_socket&&) noexcept = default;

    ~udp_socket() override = default;

    io_result read(uint8_t* data, size_t length) noexcept override;
    io_result write(const uint8_t* data, size_t length) noexcept override;
};

}
