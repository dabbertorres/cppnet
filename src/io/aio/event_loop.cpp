#include "io/aio/event_loop.hpp"

#include <cerrno>
#include <cstddef>
#include <ctime>
#include <system_error>

#include "io/io.hpp"

#include "exception.hpp"
#include "listen.hpp"

#if defined(__linux__)

#    define AIO_IO_URING

// clang-format off
#elif (defined(__APPLE__) && defined(__MACH__)) \
    || defined(__FreeBSD__) \
    || defined(__NetBSD__) \
    || defined(__OpenBSD__) \
    || defined(__bsdi__) \
    || defined(__DragonFly__)
// clang-format on

#    include <sys/event.h>
#    define AIO_KQUEUE

#elif defined(_WIN32) || defined(_WIN64) || defined(__CYGWIN__)

#    define AIO_COMPLETION_PORTS

#else
#    error "unsupported OS target"
#endif

namespace net::io::aio
{

using operation = event_loop::operation;

event_loop::event_loop()
#if defined(AIO_IO_URING)
#elif defined(AIO_KQUEUE)
    : queue(kqueue())
#elif defined(AIO_COMPLETION_PORTS)
#endif
{
    if (queue == -1) throw system_error_from_errno(errno);
}

coro::generator<tcp_socket> event_loop::accept(const listener* listener)
{
    struct kevent event = {
        .ident  = static_cast<uintptr_t>(listener->native_handle()),
        .filter = EVFILT_READ,
        .flags  = EV_ADD | EV_CLEAR,
        .fflags = 0,
        .data   = 0,
        .udata  = nullptr,
    };

    int err = kevent(queue, &event, 1, nullptr, 0, nullptr);
    if (err == -1) throw system_error_from_errno(errno);
    if ((event.flags & EV_ERROR) != 0) throw system_error_from_errno(static_cast<int>(event.data));

    struct kevent trigger; // NOLINT(cppcoreguidelines-pro-type-member-init)

    while (true)
    {
        int ret = kevent(queue, nullptr, 0, &trigger, 1, nullptr);
        if (ret == -1) throw system_error_from_errno(ret);

        for (auto i = 0; i < trigger.data; ++i)
        {
            co_yield listener->accept();
        }
    }
}

operation event_loop::write(io::writer* writer, const std::byte* data, std::size_t length)
{
    return write(writer, data, length, nullptr);
}

operation event_loop::write(io::writer* writer, const char* data, std::size_t length)
{
    return write(writer, reinterpret_cast<const std::byte*>(data), length, nullptr);
}

operation event_loop::write(io::writer* writer, std::string_view data)
{
    return write(writer, reinterpret_cast<const std::byte*>(data.data()), data.length(), nullptr);
}

operation
event_loop::write(io::writer* writer, const std::byte* data, std::size_t length, std::chrono::microseconds timeout)
{
    struct timespec wait_time = {
        .tv_sec  = std::chrono::duration_cast<std::chrono::seconds>(timeout).count(),
        .tv_nsec = std::chrono::duration_cast<std::chrono::nanoseconds>(timeout % 1s).count(),
    };
    return write(writer, data, length, &wait_time);
}

operation event_loop::write(io::writer* writer, const char* data, std::size_t length, std::chrono::microseconds timeout)
{
    return write(writer, reinterpret_cast<const std::byte*>(data), length, timeout);
}

operation event_loop::write(io::writer* writer, std::string_view data, std::chrono::microseconds timeout)
{
    return write(writer, reinterpret_cast<const std::byte*>(data.data()), data.length(), timeout);
}

operation event_loop::read(io::reader* reader, std::byte* data, std::size_t length)
{
    return read(reader, data, length, nullptr);
}

operation event_loop::read(io::reader* reader, char* data, std::size_t length)
{
    return read(reader, reinterpret_cast<std::byte*>(data), length, nullptr);
}

operation event_loop::read(io::reader* reader, std::byte* data, std::size_t length, std::chrono::microseconds timeout)
{
    struct timespec wait_time = {
        .tv_sec  = std::chrono::duration_cast<std::chrono::seconds>(timeout).count(),
        .tv_nsec = std::chrono::duration_cast<std::chrono::nanoseconds>(timeout % 1s).count(),
    };
    return read(reader, data, length, &wait_time);
}

operation event_loop::read(io::reader* reader, char* data, std::size_t length, std::chrono::microseconds timeout)
{
    return read(reader, reinterpret_cast<std::byte*>(data), length, timeout);
}

operation event_loop::write(io::writer* writer, const std::byte* data, std::size_t length, struct timespec* timeout)
{
    struct kevent event = {
        .ident  = static_cast<uintptr_t>(writer->native_handle()),
        .filter = EVFILT_WRITE,
        .flags  = EV_ADD | EV_CLEAR,
        .fflags = 0,
        .data   = 0,
        .udata  = nullptr,
    };

    int err = kevent(queue, &event, 1, nullptr, 0, nullptr);
    if (err == -1) co_return io::result{.err = std::make_error_condition(static_cast<std::errc>(errno))};
    if ((event.flags & EV_ERROR) != 0)
        co_return io::result{.err = std::make_error_condition(static_cast<std::errc>(event.data))};

    struct kevent trigger; // NOLINT(cppcoreguidelines-pro-type-member-init)

    std::size_t total_written = 0;

    while (total_written < length)
    {
        int ret = kevent(queue, nullptr, 0, &trigger, 1, timeout);
        if (ret == -1)
            co_return io::result{
                .count = total_written,
                .err   = std::make_error_condition(static_cast<std::errc>(errno)),
            };

        auto result = writer->write(data, static_cast<std::size_t>(trigger.data));
        if (result.err)
        {
            result.count += total_written;
            co_return result;
        }

        total_written += result.count;
        data += result.count;
    }

    co_return io::result{.count = length};
}

operation event_loop::read(io::reader* reader, std::byte* data, std::size_t length, struct timespec* timeout)
{
    struct kevent event = {
        .ident  = static_cast<uintptr_t>(reader->native_handle()),
        .filter = EVFILT_READ,
        .flags  = EV_ADD | EV_CLEAR,
        .fflags = 0,
        .data   = 0,
        .udata  = nullptr,
    };

    int err = kevent(queue, &event, 1, nullptr, 0, nullptr);
    if (err == -1) co_return io::result{.err = std::make_error_condition(static_cast<std::errc>(errno))};
    if ((event.flags & EV_ERROR) != 0)
        co_return io::result{.err = std::make_error_condition(static_cast<std::errc>(event.data))};

    struct kevent trigger; // NOLINT(cppcoreguidelines-pro-type-member-init)

    std::size_t total_read = 0;

    while (total_read < length)
    {
        int ret = kevent(queue, nullptr, 0, &trigger, 1, timeout);
        if (ret == -1)
            co_return io::result{
                .count = total_read,
                .err   = std::make_error_condition(static_cast<std::errc>(errno)),
            };

        auto result = reader->read(data, static_cast<std::size_t>(trigger.data));
        if (result.err)
        {
            result.count += total_read;
            co_return result;
        }

        total_read += result.count;
        data += result.count;
    }

    co_return io::result{.count = length};
}

}
