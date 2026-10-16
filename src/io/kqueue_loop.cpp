#include "config.hpp" // IWYU pragma: keep (for the platform macros)

#ifdef NET_HAS_KQUEUE

#    include "io/kqueue_loop.hpp"

#    include <algorithm>
#    include <array>
#    include <cerrno>
#    include <chrono>
#    include <cstddef>
#    include <cstdint>
#    include <ctime>
#    include <optional>
#    include <ratio>
#    include <span>
#    include <string>

#    include <sys/event.h>
#    include <unistd.h>

#    include "exception.hpp"
#    include "io/event_loop.hpp"
#    include "io/io.hpp"
#    include "io/poll.hpp"

namespace
{

// It is left as an excercise to the reader to determine if this value has any special meaning.
// (Hint: it doesn't really)
constexpr std::uintptr_t interrupt_ident = 0x6578'6974;

struct kevent make_kevent(std::uintptr_t ident, std::int16_t filter, std::uint16_t flags, std::uint32_t fflags = 0)
{
    return {
        .ident  = ident,
        .filter = filter,
        .flags  = flags,
        .fflags = fflags,
        .data   = 0,
        .udata  = nullptr,
    };
}

}

namespace net::io::detail
{

kqueue_loop::kqueue_loop()
    : descriptor{::kqueue()}
{
    if (descriptor == -1) throw system_error_from_errno(errno, "failed to create kqueue");

    // EV_CLEAR resets the event once it's been reported, so interrupts don't repeat.
    auto interrupt_event = make_kevent(interrupt_ident, EVFILT_USER, EV_ADD | EV_CLEAR);
    if (::kevent(descriptor, &interrupt_event, 1, nullptr, 0, nullptr) == -1)
    {
        auto err = errno;
        ::close(descriptor);
        throw system_error_from_errno(err, "failed to register kqueue interrupt event");
    }
}

kqueue_loop::~kqueue_loop() noexcept { ::close(descriptor); }

// NOLINTNEXTLINE(readability-make-member-function-const): modifies the kernel object.
void kqueue_loop::add(handle fd)
{
    // EV_CLEAR reports each filter when it becomes ready (edge-triggered), rather than for as long as it is.
    change(fd, EV_ADD | EV_CLEAR);
}

void kqueue_loop::remove(handle fd) noexcept
{
    try
    {
        change(fd, EV_DELETE);
    }
    catch (...) // NOLINT(bugprone-empty-catch): closing the handle removes its filters anyway.
    {}
}

// NOLINTNEXTLINE(readability-make-member-function-const): modifies the kernel object.
void kqueue_loop::change(handle fd, std::uint16_t flags)
{
    const auto ident = static_cast<std::uintptr_t>(fd);

    // EV_RECEIPT reports the result of each change, rather than failing the whole call, so that deleting a filter
    // that isn't there can be ignored.
    std::array changes{
        make_kevent(ident, EVFILT_READ, EV_RECEIPT | flags),
        make_kevent(ident, EVFILT_WRITE, EV_RECEIPT | flags),
    };
    std::array<struct kevent, changes.size()> receipts{};

    auto count = ::kevent(descriptor,
                          changes.data(),
                          static_cast<int>(changes.size()),
                          receipts.data(),
                          static_cast<int>(receipts.size()),
                          nullptr);
    if (count == -1) throw system_error_from_errno(errno, "failed to update kqueue filters");

    for (const auto& receipt : std::span{receipts}.first(static_cast<std::size_t>(count)))
    {
        if ((receipt.flags & EV_ERROR) == 0 || receipt.data == 0 || receipt.data == ENOENT) continue;

        throw system_error_from_errno(static_cast<int>(receipt.data), "failed to update kqueue filter");
    }
}

// NOLINTNEXTLINE(readability-make-member-function-const): modifies the kernel object.
std::size_t kqueue_loop::wait(std::span<ready_event> events, std::optional<std::chrono::nanoseconds> timeout)
{
    std::array<struct kevent, 64> kevents{};
    const auto                    max_events = std::min(kevents.size(), events.size());

    std::optional<timespec> wait_for;
    if (timeout)
    {
        auto secs = std::chrono::duration_cast<std::chrono::seconds>(*timeout);
        wait_for  = timespec{
             .tv_sec  = static_cast<decltype(timespec::tv_sec)>(secs.count()),
             .tv_nsec = static_cast<decltype(timespec::tv_nsec)>((*timeout - secs).count()),
        };
    }

    auto count = ::kevent(descriptor,
                          nullptr,
                          0,
                          kevents.data(),
                          static_cast<int>(max_events),
                          wait_for ? &*wait_for : nullptr);
    if (count == -1)
    {
        if (errno == EINTR) return 0;
        throw system_error_from_errno(errno, "failed to wait for kqueue events");
    }

    std::size_t num_ready = 0;

    for (const auto& kev : std::span{kevents}.first(static_cast<std::size_t>(count)))
    {
        // EV_EOF and errors are still reported as ready: the next read or write will find out what happened.
        switch (kev.filter)
        {
        case EVFILT_READ: events[num_ready++] = {.fd = static_cast<handle>(kev.ident), .ready = poll_op::read}; break;
        case EVFILT_WRITE: events[num_ready++] = {.fd = static_cast<handle>(kev.ident), .ready = poll_op::write}; break;
        default: break; // interrupts only need to wake us up.
        }
    }

    return num_ready;
}

// NOLINTNEXTLINE(readability-make-member-function-const): modifies the kernel object.
void kqueue_loop::interrupt() noexcept
{
    auto trigger = make_kevent(interrupt_ident, EVFILT_USER, 0, NOTE_TRIGGER);

    // NOTE: this can only fail if the kqueue itself is gone.
    ::kevent(descriptor, &trigger, 1, nullptr, 0, nullptr);
}

}

#endif
