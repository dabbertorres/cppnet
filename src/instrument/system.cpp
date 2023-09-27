#include "instrument/system.hpp"

#include <chrono>
#include <cstddef>

#include "config.hpp"

#ifdef NET_IS_LINUX

namespace net::instrument::system
{

using namespace std::chrono_literals;

std::chrono::seconds process_cpu_seconds() noexcept { return 0s; }

std::size_t process_open_fds() noexcept { return 0; }

std::size_t process_max_fds() noexcept { return 0; }

std::size_t process_virtual_memory() noexcept { return 0; }

std::size_t process_virtual_memory_max() noexcept { return 0; }

std::size_t process_resident_memory() noexcept { return 0; }

std::size_t process_heap() noexcept { return 0; }

time_point process_start_time() noexcept { return time_point::min(); }

std::size_t process_threads() noexcept { return 0; }

}

#elifdef NET_IS_OSX

#    include <libproc.h>

#    include <sys/resource.h>

namespace net::instrument::system
{

using namespace std::chrono_literals;

std::chrono::seconds process_cpu_seconds() noexcept { return 0s; }

std::size_t process_open_fds() noexcept { return 0; }

std::size_t process_max_fds() noexcept { return 0; }

std::size_t process_virtual_memory() noexcept { return 0; }

std::size_t process_virtual_memory_max() noexcept { return 0; }

std::size_t process_resident_memory() noexcept { return 0; }

std::size_t process_heap() noexcept { return 0; }

time_point process_start_time() noexcept { return time_point::min(); }

std::size_t process_threads() noexcept { return 0; }

}

#elifdef NET_IS_BSD

namespace net::instrument::system
{

using namespace std::chrono_literals;

std::chrono::seconds process_cpu_seconds() noexcept { return 0s; }

std::size_t process_open_fds() noexcept { return 0; }

std::size_t process_max_fds() noexcept { return 0; }

std::size_t process_virtual_memory() noexcept { return 0; }

std::size_t process_virtual_memory_max() noexcept { return 0; }

std::size_t process_resident_memory() noexcept { return 0; }

std::size_t process_heap() noexcept { return 0; }

time_point process_start_time() noexcept { return time_point::min(); }

std::size_t process_threads() noexcept { return 0; }

}

#elifdef NET_IS_WINDOWS

namespace net::instrument::system
{

using namespace std::chrono_literals;

std::chrono::seconds process_cpu_seconds() noexcept { return 0s; }

std::size_t process_open_fds() noexcept { return 0; }

std::size_t process_max_fds() noexcept { return 0; }

std::size_t process_virtual_memory() noexcept { return 0; }

std::size_t process_virtual_memory_max() noexcept { return 0; }

std::size_t process_resident_memory() noexcept { return 0; }

std::size_t process_heap() noexcept { return 0; }

time_point process_start_time() noexcept { return time_point::min(); }

std::size_t process_threads() noexcept { return 0; }

}

#else

#    error "system resources not implemented for current OS"

#endif
