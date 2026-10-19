#pragma once

#include <chrono>
#include <coroutine>
#include <cstddef>
#include <optional>
#include <span>
#include <stop_token>
#include <string>

#include <sys/socket.h>

#include "coro/task.hpp"
#include "io/io.hpp"
#include "io/reader.hpp"
#include "io/scheduler.hpp"
#include "io/writer.hpp"

namespace net
{

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
    //
    // A read deadline also applies to a read that's already waiting (moving its timer), and may be set from another
    // thread than the reading one, while it waits: e.g. by a server that takes a read another task began over, as its
    // wait for the next request. (A write deadline applies to the writes that begin after it's set.)
    void set_deadline(std::optional<io::clock::time_point> deadline) noexcept;
    void set_read_deadline(std::optional<io::clock::time_point> deadline) noexcept;
    void set_write_deadline(std::optional<io::clock::time_point> deadline) noexcept;

    // Timeouts limit how long a read or write may go without progress, waiting for the socket to be ready (then failing
    // with std::errc::timed_out), unlike a deadline, which limits them altogether. Both apply, if set. std::nullopt
    // removes it.
    void set_timeout(std::optional<io::clock::duration> timeout) noexcept;
    void set_read_timeout(std::optional<io::clock::duration> timeout) noexcept;
    void set_write_timeout(std::optional<io::clock::duration> timeout) noexcept;

    // set_stop_token makes reads and writes that wait for the socket end, failing with std::errc::operation_canceled,
    // when stop is requested (as a deadline passing does: so, like a deadline, it applies to the waits that begin after
    // it's set, and a read or write that needn't wait isn't affected). A read canceled while it waits has read nothing,
    // so the next read gets what it would have; a write canceled while it waits returns how much it wrote. A
    // default-constructed token (the default) removes it. Requesting stop must not happen holding one of the
    // scheduler's locks (see io::scheduler::stoppable_wait_operation).
    void set_stop_token(std::stop_token stop) noexcept;

    coro::task<io::result> read(std::span<std::byte> data) noexcept override;
    coro::task<io::result> write(std::span<const std::byte> data) noexcept override;

    using io::reader::read;
    using io::writer::write;

    [[nodiscard]] io::handle native_handle() const noexcept override { return fd; }

    // get_scheduler is the scheduler the socket waits with (e.g. for its pool, to resume what waits alongside it).
    [[nodiscard]] io::scheduler* get_scheduler() const noexcept { return scheduler; }

    // close closes the socket, cancelling any reads and writes waiting for it. The kernel still sends what's been
    // written, after it's closed. If graceful, close waits (up to graceful_timeout) for that to be acknowledged:
    // NOTE: blocking the calling thread, even though the socket is non-blocking (as Linux's close() does), so not
    //       on one of the scheduler's workers. The destructor doesn't.
    void close(bool graceful = false, std::chrono::seconds graceful_timeout = std::chrono::seconds{5}) noexcept;

    // shutdown_write ends the outgoing side of the connection (the peer reads the end of the stream), while reads
    // carry on.
    void shutdown_write() const noexcept;

    // shutdown_read ends the incoming side of the connection: reads (including one waiting) find the end of the
    // stream, while writes carry on. It may be called from another thread than the one reading.
    void shutdown_read() const noexcept;

protected:
    static constexpr io::handle invalid_fd = -1;

    template<typename T>
    static bool set_option(io::handle fd, int flag, T* value) noexcept
    {
        int sts = ::setsockopt(fd, SOL_SOCKET, flag, value, sizeof(T));
        return sts == 0;
    }

private:
    // read_operation and write_operation read and write once the socket's ready, which the scheduler does (see
    // io::scheduler::handle_operation), for read() and write() to return as a task when their first try would have to
    // wait, rather than a coroutine that waits and tries again in turn. A socket has one of each, as only one task may
    // wait to read it (and one to write it) at a time: claim() fails while one is.
    class read_operation final
        : public coro::operation<io::result>
        , public io::scheduler::handle_operation
    {
    public:
        explicit read_operation(socket* sock) noexcept
            : sock{sock}
        {}

        // claim readies it to read into into, returning false if it's already in use.
        [[nodiscard]] bool claim(std::span<std::byte> into) noexcept;

        // set_read_deadline is the socket's: the operation's waits are limited by it (see limit_deadline()).
        void set_read_deadline(std::optional<io::clock::time_point> deadline) noexcept;

        // take_deadline takes other's read deadline, as its socket's moved: neither may be waiting.
        void take_deadline(read_operation& other) noexcept;

        bool       start(std::coroutine_handle<> awaiting) noexcept override;
        io::result result() noexcept override;
        void       cancel() noexcept override;
        bool       perform() noexcept override;

    private:
        socket*                              sock;
        std::span<std::byte>                 data;
        std::optional<io::clock::time_point> deadline;
        io::result                           res;
    };

    class write_operation final
        : public coro::operation<io::result>
        , public io::scheduler::handle_operation
    {
    public:
        explicit write_operation(socket* sock) noexcept
            : sock{sock}
        {}

        // claim readies it to write the rest of from, after the already written, returning false if it's already in
        // use.
        [[nodiscard]] bool claim(std::span<const std::byte> from, std::size_t already) noexcept;

        bool       start(std::coroutine_handle<> awaiting) noexcept override;
        io::result result() noexcept override;
        void       cancel() noexcept override;
        bool       perform() noexcept override;

    private:
        socket*                              sock;
        std::span<const std::byte>           data;
        std::size_t                          written = 0;
        std::optional<io::clock::time_point> deadline;
        std::optional<io::clock::duration>   timeout;
        io::result                           res;
    };

    // try_read reads, unless it would have to wait (then returning nothing).
    [[nodiscard]] std::optional<io::result> try_read(std::span<std::byte> data) const noexcept;

    // try_write writes from data[written], counting what it writes in written, unless it would have to wait for the
    // rest (then returning nothing).
    [[nodiscard]] std::optional<io::result> try_write(std::span<const std::byte> data,
                                                      std::size_t&               written) const noexcept;

    // NOTE: the read deadline is the read operation's (see read_operation::set_read_deadline()).
    io::scheduler*                       scheduler;
    io::handle                           fd;
    std::optional<io::clock::time_point> write_deadline;
    std::optional<io::clock::duration>   read_timeout;
    std::optional<io::clock::duration>   write_timeout;
    std::stop_token                      stop;

    // NOTE: not moved with the socket: one mustn't be moved while a read or write waits for it.
    read_operation  reading{this};
    write_operation writing{this};
};

}
