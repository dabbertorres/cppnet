#pragma once

#include <chrono>
#include <optional>
#include <string>

#include <sys/socket.h>

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
    socket(io::scheduler* scheduler, io::handle fd);

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

    // Deadlines limit how long reads and writes wait for the socket to be ready: once one has passed, they fail with
    // std::errc::timed_out (until the deadline is changed). std::nullopt removes it.
    void set_deadline(std::optional<io::clock::time_point> deadline) noexcept;
    void set_read_deadline(std::optional<io::clock::time_point> deadline) noexcept;
    void set_write_deadline(std::optional<io::clock::time_point> deadline) noexcept;

    // Timeouts limit how long each wait of a read or write may be, i.e. how long it may go without the socket being
    // ready (then failing with std::errc::timed_out), unlike a deadline, which limits them altogether. Both apply, if
    // set. std::nullopt removes it.
    void set_timeout(std::optional<io::clock::duration> timeout) noexcept;
    void set_read_timeout(std::optional<io::clock::duration> timeout) noexcept;
    void set_write_timeout(std::optional<io::clock::duration> timeout) noexcept;

    coro::task<io::result> read(std::span<std::byte> data) noexcept override;
    coro::task<io::result> write(std::span<const std::byte> data) noexcept override;

    using io::reader::read;
    using io::writer::write;

    [[nodiscard]] io::handle native_handle() const noexcept override { return fd; }

    void close(bool graceful = true, std::chrono::seconds graceful_timeout = 5s) noexcept;

    // shutdown_write ends the outgoing side of the connection (the peer reads the end of the stream), while reads
    // carry on.
    void shutdown_write() const noexcept;

protected:
    static constexpr int invalid_fd = -1;

    template<typename T>
    static bool set_option(int fd, int flag, T* value) noexcept
    {
        int sts = ::setsockopt(fd, SOL_SOCKET, flag, value, sizeof(T));
        return sts == 0;
    }

private:
    // try_read reads, unless it would have to wait (then returning nothing).
    [[nodiscard]] std::optional<io::result> try_read(std::span<std::byte> data) const noexcept;

    // try_write writes from data[written], counting what it writes in written, unless it would have to wait for the
    // rest (then returning nothing).
    [[nodiscard]] std::optional<io::result> try_write(std::span<const std::byte> data,
                                                      std::size_t&               written) const noexcept;

    // read_waiting and write_waiting wait for the socket to be ready, then read or write, as often as it takes.
    coro::task<io::result> read_waiting(std::span<std::byte> data) noexcept;
    coro::task<io::result> write_waiting(std::span<const std::byte> data, std::size_t written) noexcept;

    io::scheduler*                       scheduler;
    int                                  fd;
    std::optional<io::clock::time_point> read_deadline;
    std::optional<io::clock::time_point> write_deadline;
    std::optional<io::clock::duration>   read_timeout;
    std::optional<io::clock::duration>   write_timeout;
};

}
