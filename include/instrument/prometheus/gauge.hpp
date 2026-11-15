#pragma once

#include <atomic>
#include <chrono>

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

    double increment(double v = 1.0) noexcept { return detail::add(value, v); }

    double decrement(double v = 1.0) noexcept { return detail::add(value, -v); }

    void set(double v) noexcept { value.store(v, std::memory_order_release); }

    double operator+=(double v) noexcept;
    double operator++() noexcept;
    double operator++(int) noexcept;
    double operator-=(double v) noexcept;
    double operator--() noexcept;
    double operator--(int) noexcept;

    auto track() noexcept
    {
        return tracker{[this](std::chrono::duration<double> elapsed) { set(elapsed.count()); }};
    }

private:
    std::atomic<double> value = 0.0;
};

}
