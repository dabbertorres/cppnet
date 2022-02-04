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
    tcp_socket(int fd, size_t buf_size = 256);

    tcp_socket(std::string_view          host,
               std::string_view          port,
               addr_protocol             proto    = addr_protocol::not_care,
               size_t                    buf_size = 256,
               std::chrono::microseconds timeout  = 5s);

    virtual ~tcp_socket() = default;

    io_result            read(uint8_t* data, size_t length) noexcept override;
    io_result            write(const uint8_t* data, size_t length) noexcept override;
    std::error_condition flush() noexcept override;
};

}
