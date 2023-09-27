#pragma once

#include <chrono>
#include <cstddef>

namespace net::instrument::system
{

using time_point = std::chrono::time_point<std::chrono::high_resolution_clock>;

struct process_info
{
    std::chrono::seconds user_cpu_time;
    std::chrono::seconds system_cpu_time;

    std::size_t max_resident_set_size;
    std::size_t page_reclaims;
    std::size_t page_faults;
    std::size_t swap_count;
    std::size_t num_blocking_input_ops;
    std::size_t num_blocking_output_ops;
    std::size_t num_voluntary_context_switches;
    std::size_t num_involuntary_context_switches;
};

std::chrono::seconds process_cpu_seconds() noexcept;
std::size_t          process_open_fds() noexcept;
std::size_t          process_max_fds() noexcept;
std::size_t          process_virtual_memory() noexcept;
std::size_t          process_virtual_memory_max() noexcept;
std::size_t          process_resident_memory() noexcept;
std::size_t          process_heap() noexcept;
time_point           process_start_time() noexcept;
std::size_t          process_threads() noexcept;

}
