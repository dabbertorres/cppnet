#pragma once

#include <atomic>

#include "io/writer.hpp"

#include "metric.hpp"
#include "tracker.hpp"

namespace net::instrument::prometheus
{

struct gauge : public base_metric<gauge>
{
    static constexpr metric_type type() noexcept { return metric_type::gauge; }

    using base_metric::base_metric;

    gauge(const gauge& other);
    gauge& operator=(const gauge& other);

    gauge(gauge&& other) noexcept;
    gauge& operator=(gauge&& other) noexcept;

    ~gauge() = default;

    [[nodiscard]] double get() const noexcept;

    template<typename Clock = std::chrono::steady_clock, typename Duration = Clock::duration>
    double increment(double v = 1.0, std::chrono::time_point<Clock, Duration> updated_at = Clock::now()) noexcept
    {
        auto current   = value.load(std::memory_order_acquire);
        auto new_value = current + v;
        value.store(new_value, std::memory_order_release);
        set_updated_at(updated_at);
        return new_value;
    }

    template<typename Clock = std::chrono::steady_clock, typename Duration = Clock::duration>
    double decrement(double v = 1.0, std::chrono::time_point<Clock, Duration> updated_at = Clock::now()) noexcept
    {
        auto current   = value.load(std::memory_order_acquire);
        auto new_value = current - v;
        value.store(new_value, std::memory_order_release);
        set_updated_at(updated_at);
        return new_value;
    }

    template<typename Clock = std::chrono::steady_clock, typename Duration = Clock::duration>
    void set(double v, std::chrono::time_point<Clock, Duration> updated_at = Clock::now()) noexcept
    {
        value.store(v, std::memory_order_release);
        set_updated_at(updated_at);
    }

    double operator+=(double v) noexcept;
    double operator++() noexcept;
    double operator++(int) noexcept;
    double operator-=(double v) noexcept;
    double operator--() noexcept;
    double operator--(int) noexcept;

    auto track() noexcept
    {
        return tracker{[this](std::chrono::seconds seconds) { set(static_cast<double>(seconds.count())); }};
    }

private:
    friend struct base_metric<gauge>;

    io::result encode_value(io::writer& out) const;

    std::atomic<double> value = 0.0;
};

}
