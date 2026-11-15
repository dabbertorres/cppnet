#pragma once

#include <atomic>

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

    double increment(double v = 1.0) noexcept { return detail::add(value, v); }

    // set replaces the value, for a counter that mirrors a count kept elsewhere (e.g. by the OS, see
    // register_process_metrics). The value mustn't decrease.
    void set(double v) noexcept { value.store(v, std::memory_order_release); }

    double operator+=(double v) noexcept;
    double operator++() noexcept;
    double operator++(int) noexcept;

private:
    std::atomic<double> value = 0.0;
};

}
