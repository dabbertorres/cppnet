#pragma once

#include <atomic>
#include <chrono>

#include "coro/task.hpp"
#include "io/io.hpp"
#include "io/writer.hpp"
#include "metric.hpp"

namespace net::instrument::prometheus
{

struct counter : public base_metric<counter>
{
    static constexpr metric_type type() noexcept { return metric_type::counter; }

    using base_metric::base_metric;

    counter(const counter& other);
    counter& operator=(const counter& other);

    counter(counter&& other) noexcept;
    counter& operator=(counter&& other) noexcept;

    ~counter() = default;

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

    double operator+=(double v) noexcept;
    double operator++() noexcept;
    double operator++(int) noexcept;

private:
    friend struct base_metric<counter>;

    coro::task<io::result> encode_value(io::writer& out) const;

    std::atomic<double> value = 0.0;
};

}
