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

class socket
    : public io::reader
    , public io::writer
{
public:
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

    io::result read(io::byte* data, size_t length) noexcept override;
    io::result write(const io::byte* data, size_t length) noexcept override;

    void close(bool graceful = true, std::chrono::seconds graceful_timeout = 5s) const noexcept;

protected:
    static constexpr int invalid_fd = -1;

    template<typename T>
    static bool set_option(int fd, int flag, T* value) noexcept
    {
        int sts = ::setsockopt(fd, SOL_SOCKET, flag, value, sizeof(T));
        return sts == 0;
    }

private:
    int fd;
};

}
