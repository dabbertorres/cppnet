#pragma once

#include <cstddef>
#include <cstdint>
#include <system_error>
#include <type_traits>

namespace net::io
{

// handle is a file descriptor. (Windows, whose IOCP event loop isn't implemented, would need a SOCKET.)
using handle = int;

// result is what a read or a write did: how many bytes it moved, and the error that stopped it, if any (which may come
// with a count, e.g. the final bytes of a stream). Errors are std::error_codes: from the OS (in std::system_category(),
// e.g. ECONNRESET), errc's, or another part of the library's (e.g. tls::errc). Compare them against conditions, which
// match every code that means the same: std::errc (e.g. err == std::errc::timed_out), or status_condition (e.g.
// err == status_condition::closed, for however the stream ended).
struct result
{
    std::size_t     count{};
    std::error_code err{};
};

// errc are the I/O errors that aren't the OS's.
enum class errc : std::uint8_t
{
    // the stream ended: there's nothing more to read (or, for a writer, it can't take any more).
    end_of_stream = 1,
    // a reader or writer returned nothing, and no error (which would otherwise repeat forever).
    no_progress = 2,
};

[[nodiscard]] const std::error_category& category() noexcept;

[[nodiscard]] std::error_code make_error_code(errc err) noexcept;

// status_condition groups the errors that mean the same to most callers.
enum class status_condition : std::uint8_t
{
    // the stream is over: it ended (errc::end_of_stream), or the connection was reset, aborted, or isn't connected.
    closed = 1,
    // the operation timed out (std::errc::timed_out).
    timed_out = 2,
};

[[nodiscard]] const std::error_category& status_condition_category() noexcept;

[[nodiscard]] std::error_condition make_error_condition(status_condition cond) noexcept;
}

namespace std
{

template<>
struct is_error_code_enum<net::io::errc> : std::true_type
{};

template<>
struct is_error_condition_enum<net::io::status_condition> : std::true_type
{};

}
