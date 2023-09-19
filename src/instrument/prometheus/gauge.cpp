#include "instrument/prometheus/gauge.hpp"

#include <atomic>
#include <chrono>

#include "io/writer.hpp"

namespace net::instrument::prometheus
{

gauge::gauge(const gauge& other)
    : base_metric{other}
    , value{other.value.load(std::memory_order_acquire)}
{}

gauge& gauge::operator=(const gauge& other)
{
    if (this == std::addressof(other)) return *this;

    value.store(other.value.load(std::memory_order_acquire), std::memory_order_release);
    static_cast<base_metric*>(this)->operator=(other);
    return *this;
}

gauge::gauge(gauge&& other) noexcept
    : base_metric{std::move(other)}
    , value{other.value.load(std::memory_order_acquire)}
{}

gauge& gauge::operator=(gauge&& other) noexcept
{
    value.store(other.value.load(std::memory_order_acquire), std::memory_order_release);
    static_cast<base_metric*>(this)->operator=(std::move(other));
    return *this;
}

double gauge::get() const noexcept { return value.load(std::memory_order_acquire); }

double gauge::operator+=(double v) noexcept { return increment(v); }
double gauge::operator++() noexcept { return increment(1); }
double gauge::operator++(int) noexcept { return increment(1) - 1; }
double gauge::operator-=(double v) noexcept { return decrement(v); }
double gauge::operator--() noexcept { return decrement(1); }
double gauge::operator--(int) noexcept { return decrement(1) - 1; }

io::result gauge::encode_value(io::writer& out) const
{
    auto val = value.load(std::memory_order_acquire);
    return out.write(std::to_string(val));
}

}
