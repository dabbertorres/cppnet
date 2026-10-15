#pragma once

#include <chrono>
#include <cstddef>
#include <expected>
#include <system_error>

// Statistics about this process: those of Prometheus' standard process_* metrics, and getrusage()'s.
//
// Each fails with std::errc::function_not_supported where it isn't implemented (on platforms other than Linux and
// macOS, except for those from POSIX), and otherwise with the error that reading it failed with (e.g. std::errc::
// bad_message if /proc's format wasn't as expected).
namespace net::instrument::system
{

using time_point = std::chrono::system_clock::time_point;

// unlimited is what a limit is when there is none.
inline constexpr std::size_t unlimited = static_cast<std::size_t>(-1);

// process_cpu_seconds is how much CPU time the process has used (in user and system mode).
std::expected<std::chrono::duration<double>, std::error_condition> process_cpu_seconds() noexcept;

// process_open_fds is how many file descriptors the process has open.
std::expected<std::size_t, std::error_condition> process_open_fds() noexcept;

// process_max_fds is how many file descriptors the process may have open (its soft limit), or unlimited.
std::expected<std::size_t, std::error_condition> process_max_fds() noexcept;

// process_virtual_memory is the size of the process' virtual memory, in bytes.
std::expected<std::size_t, std::error_condition> process_virtual_memory() noexcept;

// process_virtual_memory_max is how large the process' virtual memory may be, in bytes (its soft limit), or
// unlimited.
std::expected<std::size_t, std::error_condition> process_virtual_memory_max() noexcept;

// process_resident_memory is how much of the process' memory is resident (in RAM), in bytes.
std::expected<std::size_t, std::error_condition> process_resident_memory() noexcept;

// process_heap is how much memory the process has allocated with malloc (and so new), in bytes, as the allocator
// counts it: glibc's (with mallinfo2) on Linux, or the malloc zones' on macOS.
//
// NOTE: a sanitizer's allocator, if any, may not count it.
std::expected<std::size_t, std::error_condition> process_heap() noexcept;

// process_start_time is when the process started.
std::expected<time_point, std::error_condition> process_start_time() noexcept;

// process_threads is how many threads the process has.
std::expected<std::size_t, std::error_condition> process_threads() noexcept;

// process_info is the process' resource usage, as getrusage() reports it.
struct process_info
{
    std::chrono::duration<double> user_cpu_time;
    std::chrono::duration<double> system_cpu_time;

    // in bytes (on every platform: Linux reports kilobytes, and macOS bytes).
    std::size_t max_resident_set_size;

    // minor page faults (those that didn't need I/O), and major (those that did).
    std::size_t page_reclaims;
    std::size_t page_faults;
    std::size_t swap_count;
    std::size_t num_blocking_input_ops;
    std::size_t num_blocking_output_ops;
    std::size_t num_voluntary_context_switches;
    std::size_t num_involuntary_context_switches;
};

std::expected<process_info, std::error_condition> process_usage() noexcept;

}
