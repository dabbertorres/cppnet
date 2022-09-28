#pragma once

#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <sys/socket.h>

#include "io/io.hpp"
#include "io/reader.hpp"
#include "io/writer.hpp"

namespace net
{

using namespace std::chrono_literals;

enum class protocol
{
    not_care,
    ipv4,
    ipv6,
};

class socket
    : public io::reader<char>
    , public io::writer<char>
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

    io::result read(char* data, size_t length) noexcept override;
    io::result write(const char* data, size_t length) noexcept override;

    void close(bool graceful = true, std::chrono::seconds graceful_timeout = 5s) noexcept;

protected:
    static constexpr int invalid_fd = -1;

    template<typename T>
    bool set_option(int flag, T* value) noexcept
    {
        int sts = ::setsockopt(fd, SOL_SOCKET, flag, value, sizeof(T));
        return sts == 0;
    }

    int fd;
};

}
