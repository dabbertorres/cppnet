#pragma once

#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
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

namespace detail
{
    struct socket_address;
}

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

    // local_port is the port the socket is bound to (e.g. the one the OS chose, for one bound to port 0, or connected
    // without binding). It throws std::system_error if the OS can't say, or the socket isn't IPv4 or IPv6.
    [[nodiscard]] std::uint16_t local_port() const;

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

    // wait_writable waits until the socket is writable by the kernel's reckoning: a write would take something without
    // waiting; for a TCP socket with a mark (see tcp_socket::set_notsent_lowat()), what's written but not yet sent is
    // under it too (macOS's poll() then looks only at the mark, so a write may still have to wait for the send buffer,
    // as writes do); or a write would fail at once (e.g. the connection was reset), the write then saying why. Its
    // result's count is 0 (as a write of nothing's), and its err why it ended otherwise. It asks the kernel first
    // (poll(), which doesn't wait), returning without a coroutine (see coro::ready_task) if it's writable; otherwise
    // it waits as a write does, through the socket's write operation, so the write deadline, timeout and stop token
    // apply (std::errc::timed_out, std::errc::operation_canceled), and it can't wait alongside a write: whichever
    // comes second fails with std::errc::device_or_resource_busy, as a second write would. (As the scheduler's events
    // are edge-triggered, a readiness it kept from earlier may be stale: the kernel's asked again each time the socket
    // is reported, and the wait goes on until it says writable.)
    coro::task<io::result> wait_writable() noexcept;

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

    // read_from is read() for a datagram socket, which also gives the datagram's sender in from: as one datagram is
    // read at a time, an empty one is read as such, rather than as the end of the stream. from must outlive the read.
    coro::task<io::result> read_from(std::span<std::byte> data, detail::socket_address& from) noexcept;

    // write_to is write() for a datagram socket, sending data as one datagram to to (rather than to the socket's peer),
    // even if it's empty. to must outlive the write.
    coro::task<io::result> write_to(std::span<const std::byte> data, const detail::socket_address& to) noexcept;

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

        // claim readies it to read into into (and, if from isn't null, the sender's address into from: see
        // read_from()), returning false if it's already in use.
        [[nodiscard]] bool claim(std::span<std::byte> into, detail::socket_address* from = nullptr) noexcept;

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
        detail::socket_address*              from = nullptr;
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

        // claim readies it to write the rest of from, after the already written (or, if to isn't null, all of from as
        // one datagram to to: see write_to()), returning false if it's already in use.
        [[nodiscard]] bool claim(std::span<const std::byte>    from,
                                 std::size_t                   already,
                                 const detail::socket_address* to = nullptr) noexcept;

        // claim_writable readies it to wait until the socket's writable, writing nothing (see wait_writable()),
        // returning false if it's already in use.
        [[nodiscard]] bool claim_writable() noexcept;

        bool       start(std::coroutine_handle<> awaiting) noexcept override;
        io::result result() noexcept override;
        void       cancel() noexcept override;
        bool       perform() noexcept override;

    private:
        socket*                              sock;
        std::span<const std::byte>           data;
        const detail::socket_address*        to      = nullptr;
        std::size_t                          written = 0;
        std::optional<io::clock::time_point> deadline;
        std::optional<io::clock::duration>   timeout;
        io::result                           res;

        // only_writable is whether it waits until the socket's writable, rather than writing (see wait_writable()).
        bool only_writable = false;
    };

    // try_read reads, unless it would have to wait (then returning nothing). If from isn't null, it reads a datagram,
    // and its sender's address into from.
    [[nodiscard]] std::optional<io::result> try_read(std::span<std::byte>    data,
                                                     detail::socket_address* from = nullptr) const noexcept;

    // try_write writes from data[written], counting what it writes in written, unless it would have to wait for the
    // rest (then returning nothing). If to isn't null, it sends data as one datagram to to (written being 0).
    [[nodiscard]] std::optional<io::result> try_write(std::span<const std::byte>    data,
                                                      std::size_t&                  written,
                                                      const detail::socket_address* to = nullptr) const noexcept;

    // poll_writable asks the kernel whether the socket's writable (see wait_writable()), without waiting: nothing if
    // it isn't.
    [[nodiscard]] std::optional<io::result> poll_writable() const noexcept;

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
