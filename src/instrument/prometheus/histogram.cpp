#include "instrument/prometheus/histogram.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "instrument/prometheus/metric.hpp"

namespace
{

using counts_t = std::unique_ptr<std::atomic<std::uint64_t>[]>;

counts_t zeroed_counts(std::size_t size) { return std::make_unique<std::atomic<std::uint64_t>[]>(size); }

counts_t copy_counts(const counts_t& from, std::size_t size)
{
    auto counts = zeroed_counts(size);
    for (std::size_t i = 0; i < size; ++i) counts[i].store(from[i].load(std::memory_order_relaxed));
    return counts;
}

}

namespace net::instrument::prometheus
{

histogram::histogram(std::string&& name, std::string&& help, metric_labels&& labels, std::vector<double>&& buckets)
    : base_metric{std::move(name), std::move(help), std::move(labels)}
    , bounds{std::move(buckets)}
{
    // NOTE: NaN isn't increasing, but compares false with everything.
    if (std::ranges::any_of(bounds, [](double bound) { return std::isnan(bound); })
        || std::ranges::adjacent_find(bounds, std::greater_equal{}) != bounds.end())
    {
        throw std::invalid_argument{"histogram buckets must be increasing"};
    }

    // the +Inf bucket counts every observation.
    if (bounds.empty() || bounds.back() != std::numeric_limits<double>::infinity())
    {
        bounds.push_back(std::numeric_limits<double>::infinity());
    }

    counts = zeroed_counts(bounds.size());
}

histogram::histogram(const histogram& other)
    : base_metric{other}
    , bounds{other.bounds}
    , counts{copy_counts(other.counts, other.bounds.size())}
    , sum{other.sum.load(std::memory_order_relaxed)}
{}

histogram& histogram::operator=(const histogram& other)
{
    if (this == std::addressof(other)) return *this;

    base_metric::operator=(other);
    bounds = other.bounds;
    counts = copy_counts(other.counts, other.bounds.size());
    sum.store(other.sum.load(std::memory_order_relaxed), std::memory_order_relaxed);

    return *this;
}

histogram::histogram(histogram&& other) noexcept
    : base_metric{std::move(other)}
    , bounds{std::exchange(other.bounds, {})}
    , counts{std::move(other.counts)}
    , sum{other.sum.exchange(0.0, std::memory_order_relaxed)}
{}

histogram& histogram::operator=(histogram&& other) noexcept
{
    if (this == std::addressof(other)) return *this;

    base_metric::operator=(std::move(other));
    bounds = std::exchange(other.bounds, {});
    counts = std::move(other.counts);
    sum.store(other.sum.exchange(0.0, std::memory_order_relaxed), std::memory_order_relaxed);

    return *this;
}

void histogram::observe(double v) noexcept
{
    // buckets count observations less than or equal to their bound. NOTE: NaN compares false with every bound (so it
    // would be the first bucket's), but only belongs in the +Inf bucket.
    auto i = std::isnan(v) ? bounds.size() - 1
                           : static_cast<std::size_t>(std::ranges::lower_bound(bounds, v) - bounds.begin());

    counts[i].fetch_add(1, std::memory_order_relaxed);
    detail::add(sum, v);
}

void histogram::encode_self(std::string& out) const
{
    // NOTE: no timestamps, which histograms don't usually have.

    encode_help(out);
    encode_type(out);

    std::uint64_t total = 0;

    for (std::size_t i = 0; i < bounds.size(); ++i)
    {
        // each bucket's count includes those of the buckets before it.
        total += counts[i].load(std::memory_order_relaxed);

        out += name;
        out += "_bucket";
        encode_labels(out, "le", format_value(bounds[i]));
        out += ' ';
        out += std::to_string(total);
        out += '\n';
    }

    // NOTE: the sum may already include observations the buckets don't, or not yet ones they do, as it's updated
    //       separately.
    out += name;
    out += "_sum";
    encode_labels(out);
    out += ' ';
    out += format_value(sum.load(std::memory_order_relaxed));
    out += '\n';

    // the +Inf bucket's.
    out += name;
    out += "_count";
    encode_labels(out);
    out += ' ';
    out += std::to_string(total);
    out += '\n';
}

void histogram::on_derive(histogram& child)
{
    child.bounds = bounds;
    child.counts = zeroed_counts(bounds.size());
}

}
