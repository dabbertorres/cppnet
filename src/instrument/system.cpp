#include "instrument/system.hpp"

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <expected>
#include <system_error>

#include <sys/resource.h>
#include <sys/time.h>
#include <unistd.h>

#include "config.hpp"

#ifdef NET_IS_LINUX
#    include <charconv>
#    include <fstream>
#    include <iterator>
#    include <string>
#    include <string_view>

#    include <dirent.h>

#    ifdef __GLIBC__
#        include <malloc.h>
#    endif
#endif

#ifdef NET_IS_OSX
#    include <new>
#    include <vector>

#    include <libproc.h>

#    include <mach/mach.h>
#    include <malloc/malloc.h>
#endif

namespace
{

using net::instrument::system::unlimited;

template<typename T>
using result = std::expected<T, std::error_condition>;

// from_errno is the error a system call failed with (or, if it didn't set errno, std::errc::io_error).
std::unexpected<std::error_condition> from_errno() noexcept
{
    const int err = errno;
    return std::unexpected{
        err != 0 ? std::error_condition{err, std::generic_category()}
            : std::make_error_condition(std::errc::io_error)
    };
}

std::unexpected<std::error_condition> failed(std::errc err) noexcept
{
    return std::unexpected{std::make_error_condition(err)};
}

[[maybe_unused]] std::unexpected<std::error_condition> not_supported() noexcept
{
    return failed(std::errc::function_not_supported);
}

std::chrono::duration<double> to_duration(const timeval& tv) noexcept
{
    return std::chrono::seconds{tv.tv_sec} + std::chrono::microseconds{tv.tv_usec};
}

result<rusage> usage() noexcept
{
    rusage ru{};
    if (::getrusage(RUSAGE_SELF, &ru) != 0) return from_errno();
    return ru;
}

result<std::size_t> soft_limit(int resource) noexcept
{
    rlimit limit{};
    if (::getrlimit(resource, &limit) != 0) return from_errno();
    if (limit.rlim_cur == RLIM_INFINITY) return unlimited;
    return static_cast<std::size_t>(limit.rlim_cur);
}

#ifdef NET_IS_LINUX

// read_file reads a whole (small) file, such as one of /proc's.
result<std::string> read_file(const char* path) noexcept
try
{
    errno = 0;
    std::ifstream in{path};
    if (!in) return from_errno();

    std::string content{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    if (in.bad()) return from_errno();
    return content;
}
catch (const std::bad_alloc&)
{
    return failed(std::errc::not_enough_memory);
}
catch (...)
{
    return failed(std::errc::io_error);
}

// parse_number parses the whole of text as a decimal number, failing with std::errc::bad_message (as it's from a file
// that wasn't as expected) otherwise.
template<typename T>
result<T> parse_number(std::string_view text) noexcept
{
    T value{};
    auto [end, err] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (err != std::errc{} || end != text.data() + text.size()) return failed(std::errc::bad_message);
    return value;
}

// stat_field returns a field of /proc/self/stat, numbered as in proc(5) (e.g. 20 is num_threads).
template<typename T>
result<T> stat_field(std::size_t field) noexcept
{
    auto stat = read_file("/proc/self/stat");
    if (!stat) return std::unexpected{stat.error()};

    // NOTE: the second field (the command's name, in parentheses) may contain spaces and parentheses itself, so the
    //       fields are counted from after its closing parenthesis, which is the last in the line: the third field.
    auto end_of_name = stat->rfind(')');
    if (end_of_name == std::string::npos) return failed(std::errc::bad_message);

    std::string_view rest{*stat};
    rest = rest.substr(end_of_name + 1);

    for (std::size_t current = 3;; ++current)
    {
        auto begin = rest.find_first_not_of(' ');
        if (begin == std::string_view::npos) return failed(std::errc::bad_message);
        rest = rest.substr(begin);

        auto end = rest.find_first_of(" \n");
        if (current == field) return parse_number<T>(rest.substr(0, end));

        if (end == std::string_view::npos) return failed(std::errc::bad_message);
        rest = rest.substr(end);
    }
}

// statm_bytes returns a field of /proc/self/statm (0 is the size of the virtual memory, and 1 the resident), in bytes.
result<std::size_t> statm_bytes(std::size_t field) noexcept
{
    auto statm = read_file("/proc/self/statm");
    if (!statm) return std::unexpected{statm.error()};

    std::string_view rest{*statm};
    for (std::size_t current = 0; current < field; ++current)
    {
        auto space = rest.find(' ');
        if (space == std::string_view::npos) return failed(std::errc::bad_message);
        rest = rest.substr(space + 1);
    }

    auto pages = parse_number<std::size_t>(rest.substr(0, rest.find_first_of(" \n")));
    if (!pages) return std::unexpected{pages.error()};

    auto page_size = ::sysconf(_SC_PAGESIZE);
    if (page_size <= 0) return from_errno();

    return *pages * static_cast<std::size_t>(page_size);
}

#endif

#ifdef NET_IS_OSX

result<mach_task_basic_info> basic_task_info() noexcept
{
    mach_task_basic_info   info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;

    // NOTE: a kern_return_t isn't an errno.
    if (::task_info(::mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count)
        != KERN_SUCCESS)
    {
        return failed(std::errc::io_error);
    }

    return info;
}

// pid_info returns one of proc_pidinfo()'s structures about this process.
template<typename T>
result<T> pid_info(int flavor) noexcept
{
    T info{};

    errno = 0;
    if (::proc_pidinfo(::getpid(), flavor, 0, &info, sizeof(info)) != static_cast<int>(sizeof(info)))
    {
        return from_errno();
    }

    return info;
}

#endif

}

namespace net::instrument::system
{

std::expected<std::chrono::duration<double>, std::error_condition> process_cpu_seconds() noexcept
{
    return usage().transform([](const rusage& ru) { return to_duration(ru.ru_utime) + to_duration(ru.ru_stime); });
}

std::expected<std::size_t, std::error_condition> process_open_fds() noexcept
{
#if defined(NET_IS_LINUX)
    DIR* dir = ::opendir("/proc/self/fd");
    if (dir == nullptr) return from_errno();

    // NOTE: not counting the descriptor of the directory itself, which is only open to count them.
    const int own = ::dirfd(dir);

    std::size_t count = 0;
    while (const auto* entry = ::readdir(dir))
    {
        std::string_view name{entry->d_name};
        if (name == "." || name == "..") continue;

        auto fd = parse_number<int>(name);
        if (!fd || *fd != own) ++count;
    }

    ::closedir(dir);
    return count;
#elif defined(NET_IS_OSX)
    const auto pid = ::getpid();

    errno    = 0;
    int size = ::proc_pidinfo(pid, PROC_PIDLISTFDS, 0, nullptr, 0);
    if (size <= 0) return from_errno();

    try
    {
        // NOTE: more may be opened between asking how many there are and listing them, so there's room to spare.
        std::vector<proc_fdinfo> fds((static_cast<std::size_t>(size) / sizeof(proc_fdinfo)) + 16);

        errno = 0;
        size  = ::proc_pidinfo(pid, PROC_PIDLISTFDS, 0, fds.data(), static_cast<int>(fds.size() * sizeof(proc_fdinfo)));
        if (size <= 0) return from_errno();

        return static_cast<std::size_t>(size) / sizeof(proc_fdinfo);
    }
    catch (const std::bad_alloc&)
    {
        return failed(std::errc::not_enough_memory);
    }
#else
    return not_supported();
#endif
}

std::expected<std::size_t, std::error_condition> process_max_fds() noexcept { return soft_limit(RLIMIT_NOFILE); }

std::expected<std::size_t, std::error_condition> process_virtual_memory() noexcept
{
#if defined(NET_IS_LINUX)
    return statm_bytes(0);
#elif defined(NET_IS_OSX)
    return basic_task_info().transform([](const mach_task_basic_info& info)
                                       { return static_cast<std::size_t>(info.virtual_size); });
#else
    return not_supported();
#endif
}

std::expected<std::size_t, std::error_condition> process_virtual_memory_max() noexcept { return soft_limit(RLIMIT_AS); }

std::expected<std::size_t, std::error_condition> process_resident_memory() noexcept
{
#if defined(NET_IS_LINUX)
    return statm_bytes(1);
#elif defined(NET_IS_OSX)
    return basic_task_info().transform([](const mach_task_basic_info& info)
                                       { return static_cast<std::size_t>(info.resident_size); });
#else
    return not_supported();
#endif
}

std::expected<std::size_t, std::error_condition> process_heap() noexcept
{
#if defined(NET_IS_LINUX) && defined(__GLIBC__)
    // the bytes allocated: from the main heap (arena), and with mmap (for large allocations).
    auto info = ::mallinfo2();
    return info.uordblks + info.hblkhd;
#elif defined(NET_IS_OSX)
    // every zone's (with no zone).
    malloc_statistics_t stats{};
    ::malloc_zone_statistics(nullptr, &stats);
    return stats.size_in_use;
#else
    return not_supported();
#endif
}

std::expected<time_point, std::error_condition> process_start_time() noexcept
{
#if defined(NET_IS_LINUX)
    // starttime is in clock ticks since the system booted, and /proc/stat's btime is when that was, in Unix time.
    auto start_ticks = stat_field<unsigned long long>(22);
    if (!start_ticks) return std::unexpected{start_ticks.error()};

    auto ticks = ::sysconf(_SC_CLK_TCK);
    if (ticks <= 0) return from_errno();

    auto stat = read_file("/proc/stat");
    if (!stat) return std::unexpected{stat.error()};

    std::string_view rest{*stat};

    auto btime = rest.find("\nbtime ");
    if (btime == std::string_view::npos) return failed(std::errc::bad_message);
    rest = rest.substr(btime + 7);

    auto boot = parse_number<long long>(rest.substr(0, rest.find('\n')));
    if (!boot) return std::unexpected{boot.error()};

    auto since_boot = std::chrono::duration<double>{static_cast<double>(*start_ticks) / static_cast<double>(ticks)};
    return time_point{std::chrono::seconds{*boot}} + std::chrono::duration_cast<time_point::duration>(since_boot);
#elif defined(NET_IS_OSX)
    return pid_info<proc_bsdinfo>(PROC_PIDTBSDINFO)
        .transform(
            [](const proc_bsdinfo& info)
            {
                return time_point{std::chrono::seconds{info.pbi_start_tvsec}}
                     + std::chrono::duration_cast<time_point::duration>(
                           std::chrono::microseconds{info.pbi_start_tvusec});
            });
#else
    return not_supported();
#endif
}

std::expected<std::size_t, std::error_condition> process_threads() noexcept
{
#if defined(NET_IS_LINUX)
    return stat_field<std::size_t>(20);
#elif defined(NET_IS_OSX)
    return pid_info<proc_taskinfo>(PROC_PIDTASKINFO)
        .transform([](const proc_taskinfo& info) { return static_cast<std::size_t>(info.pti_threadnum); });
#else
    return not_supported();
#endif
}

std::expected<process_info, std::error_condition> process_usage() noexcept
{
#if defined(NET_IS_OSX)
    constexpr std::size_t max_rss_unit = 1; // bytes
#else
    constexpr std::size_t max_rss_unit = 1024; // kilobytes
#endif

    return usage().transform(
        [](const rusage& ru)
        {
            return process_info{
                .user_cpu_time                    = to_duration(ru.ru_utime),
                .system_cpu_time                  = to_duration(ru.ru_stime),
                .max_resident_set_size            = static_cast<std::size_t>(ru.ru_maxrss) * max_rss_unit,
                .page_reclaims                    = static_cast<std::size_t>(ru.ru_minflt),
                .page_faults                      = static_cast<std::size_t>(ru.ru_majflt),
                .swap_count                       = static_cast<std::size_t>(ru.ru_nswap),
                .num_blocking_input_ops           = static_cast<std::size_t>(ru.ru_inblock),
                .num_blocking_output_ops          = static_cast<std::size_t>(ru.ru_oublock),
                .num_voluntary_context_switches   = static_cast<std::size_t>(ru.ru_nvcsw),
                .num_involuntary_context_switches = static_cast<std::size_t>(ru.ru_nivcsw),
            };
        });
}

}
