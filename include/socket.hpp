#pragma once

#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "io/io.hpp"
#include "io/reader.hpp"
#include "io/writer.hpp"

namespace net
{

enum class protocol
{
    not_care,
    ipv4,
    ipv6,
};

class socket
    : public io::reader<std::byte>
    , public io::writer<std::byte>
{
public:
    socket();
    socket(int fd);

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

    io::result read(std::byte* data, size_t length) noexcept override;
    io::result write(const std::byte* data, size_t length) noexcept override;

protected:
    static constexpr int invalid_fd = -1;

    int fd;
};

}
