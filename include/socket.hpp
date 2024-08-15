#pragma once

#include <chrono>
#include <cstddef>
#include <span>
#include <string>

#include <sys/socket.h>

#include "coro/task.hpp"
#include "io/io.hpp"
#include "io/reader.hpp"
#include "io/scheduler.hpp"
#include "io/writer.hpp"

namespace net
{

using namespace std::chrono_literals;

class socket
    : public io::reader
    , public io::writer
{
public:
    socket(io::scheduler* scheduler, int fd);

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

    io::result read(std::span<std::byte> data) noexcept override;

    coro::task<io::result> co_read(std::span<std::byte> data) noexcept override;

    io::result write(std::span<const std::byte> data) noexcept override;

    coro::task<io::result> co_write(std::span<const std::byte> data) noexcept override;

    using io::reader::read;
    using io::writer::write;

    [[nodiscard]] io::io_handle native_handle() const noexcept override { return fd; }

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
    io::scheduler* scheduler;
    int            fd;
};

}
