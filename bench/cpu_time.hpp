#pragma once

#include <chrono>
#include <format>
#include <string>
#include <string_view>

#include <sys/resource.h>
#include <sys/time.h>

namespace bench
{

// in_us is t over count units, in us per unit.
inline double in_us(std::chrono::microseconds t, double count) noexcept
{
    return static_cast<double>(t.count()) / count;
}

// cpu_time is the CPU time the process has used (all of its threads'), user and system apart. Over a socket, a bench's
// wall time is mostly waiting (for the kernel, and the other side's round trip), so work saved shows in CPU time rather
// than in wall time: the benches that measure over sockets report both. Where a bench runs both sides in one process,
// it's both sides' (and their schedulers', whose idle workers wait in the kernel).
struct cpu_time
{
    std::chrono::microseconds user{};
    std::chrono::microseconds system{};

    // now is the process's CPU time so far (zero if it can't be read).
    static cpu_time now() noexcept
    {
        rusage usage{};
        if (::getrusage(RUSAGE_SELF, &usage) != 0) return {};

        const auto of = [](const timeval& tv)
        { return std::chrono::seconds{tv.tv_sec} + std::chrono::microseconds{tv.tv_usec}; };
        return {.user = of(usage.ru_utime), .system = of(usage.ru_stime)};
    }

    [[nodiscard]] std::chrono::microseconds total() const noexcept { return user + system; }

    cpu_time operator-(const cpu_time& other) const noexcept
    {
        return {.user = user - other.user, .system = system - other.system};
    }

    // least is whichever of this and other used less CPU time altogether (other if this is none yet).
    [[nodiscard]] cpu_time least(const cpu_time& other) const noexcept
    {
        if (total() == std::chrono::microseconds::zero() || other.total() < total()) return other;
        return *this;
    }

    // per describes the time per unit, of count units, in us: "X us CPU (user Y, system Z)".
    [[nodiscard]] std::string per(double count) const
    {
        return std::format("{:.2f} us CPU (user {:.2f}, system {:.2f})",
                           in_us(total(), count),
                           in_us(user, count),
                           in_us(system, count));
    }

    // line is a bench's line for the time per unit, of count units, aligned as its others are:
    // "     X.XX us CPU per <unit> (user Y, system Z)".
    [[nodiscard]] std::string line(double count, std::string_view unit) const
    {
        return std::format("{:>10.2f} us CPU per {} (user {:.2f}, system {:.2f})",
                           in_us(total(), count),
                           unit,
                           in_us(user, count),
                           in_us(system, count));
    }
};

}
