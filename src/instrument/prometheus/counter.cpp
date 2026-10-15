#include "instrument/prometheus/counter.hpp"

#include <atomic>
#include <memory>
#include <utility>

#include "instrument/prometheus/metric.hpp"

namespace net::instrument::prometheus
{

counter::counter(const counter& other)
    : base_metric{other}
    , value{other.value.load(std::memory_order_acquire)}
{}

counter& counter::operator=(const counter& other)
{
    if (this == std::addressof(other)) return *this;

    value.store(other.value.load(std::memory_order_acquire), std::memory_order_release);
    static_cast<base_metric*>(this)->operator=(other);
    return *this;
}

counter::counter(counter&& other) noexcept
    : base_metric{std::move(other)}
    , value{other.value.load(std::memory_order_acquire)}
{}

counter& counter::operator=(counter&& other) noexcept
{
    value.store(other.value.load(std::memory_order_acquire), std::memory_order_release);
    static_cast<base_metric*>(this)->operator=(std::move(other));
    return *this;
}

double counter::get() const noexcept { return value.load(std::memory_order_acquire); }

double counter::operator+=(double v) noexcept { return increment(v); }
double counter::operator++() noexcept { return increment(1); }
double counter::operator++(int) noexcept { return increment(1) - 1; }

}
