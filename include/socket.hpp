#pragma once

#include <chrono>
#include <string>
#include <string_view>
#include <vector>

#include "reader.hpp"
#include "writer.hpp"

namespace net
{

enum class protocol
{
    not_care,
    ipv4,
    ipv6,
};

class socket
    : public reader
    , public writer
{
public:
    socket();
    socket(int fd, size_t buf_size = 256);

    // non-copyable
    socket(const socket&)            = delete;
    socket& operator=(const socket&) = delete;

    // movable
    socket(socket&& other) noexcept;
    socket& operator=(socket&& other) noexcept;

    ~socket() override;

    [[nodiscard]] bool        valid() const noexcept;
    [[nodiscard]] std::string local_addr() const;
    [[nodiscard]] std::string remote_addr() const;

protected:
    static constexpr int invalid_fd = -1;

    socket(size_t buf_size);

    int fd;

private:
    std::vector<uint8_t> read_buf;
    std::vector<uint8_t> write_buf;
};

}
