#include "instrument/prometheus/histogram.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <version>

#include "io/writer.hpp"

#include "instrument/prometheus/metric.hpp"

namespace net::instrument::prometheus
{

histogram::histogram(std::string&& name, std::string&& help, metric_labels&& labels, std::vector<double>&& buckets)
    : base_metric{std::move(name), std::move(help), std::move(labels)}
    , buckets{buckets}
    , bucket_values(buckets.size())
{
    // ensure we have an "infinity" bucket
    if (buckets.back() != std::numeric_limits<double>::infinity())
    {
        buckets.push_back(std::numeric_limits<double>::infinity());
        bucket_values.emplace_back();
    }
}

histogram::histogram(const histogram& other)
    : base_metric{other}
    , buckets{other.buckets}
    , bucket_values(other.bucket_values.size()) // size never changes, so no need to lock
    , count{other.count.load(std::memory_order_acquire)}
    , sum{other.sum.load(std::memory_order_acquire)}
{
    std::lock_guard lock{other.mutex};
    std::copy(other.bucket_values.begin(), other.bucket_values.end(), bucket_values.begin());
}

histogram& histogram::operator=(const histogram& other)
{
    if (this == std::addressof(other)) return *this;

    std::scoped_lock lock{mutex, other.mutex};

    static_cast<base_metric*>(this)->operator=(other);
    buckets       = other.buckets;
    bucket_values = other.bucket_values;
    count.store(other.count.load(std::memory_order_acquire), std::memory_order_release);
    sum.store(other.sum.load(std::memory_order_acquire), std::memory_order_release);

    return *this;
}

histogram::histogram(histogram&& other) noexcept
    : base_metric{std::move(other)}
    , buckets{std::exchange(other.buckets, {})}
    , count{other.count.exchange(0, std::memory_order_acquire)}
    , sum{other.sum.exchange(0, std::memory_order_acquire)}
{
    std::lock_guard lock{other.mutex};

    bucket_values = std::exchange(other.bucket_values, {});
}

histogram& histogram::operator=(histogram&& other) noexcept
{
    std::scoped_lock lock{mutex, other.mutex};

    buckets       = std::exchange(other.buckets, {});
    bucket_values = std::exchange(other.bucket_values, {});
    count.store(other.count.exchange(0, std::memory_order_release), std::memory_order_release);
    sum.store(other.sum.exchange(0, std::memory_order_release), std::memory_order_release);
    static_cast<base_metric*>(this)->operator=(std::move(other));

    return *this;
}

void histogram::observe(double v) noexcept
{
    auto current_sum = sum.load(std::memory_order_acquire);
    sum.store(current_sum + v, std::memory_order_release);

    count.fetch_add(1, std::memory_order_relaxed);

#ifndef __cpp_lib_atomic_ref
    // allow (somewhat) simultaneous observations
    std::shared_lock lock{mutex};
#endif

    std::size_t i = 0;

    // find the first bucket we belong in
    while (i < buckets.size() && v > buckets[i])
    {
        ++i;
    }

    // now add to all the buckets we fit in
    while (i < buckets.size() && v < buckets[i])
    {
#ifdef __cpp_lib_atomic_ref
        std::atomic_ref ref{bucket_values[i]};
        ref.fetch_add(1, std::memory_order_relaxed);
#else
        bucket_values[i] += 1;
#endif
        ++i;
    }
}

io::result histogram::encode_self(io::writer& out) const
{
    // NOTE: not including timestamp in output - appears to not usually be included in histogram output

    return io::write_all(
        out,
        [this](io::writer& out) { return encode_help(out); },
        [this](io::writer& out) { return encode_type(out); },
        [this](io::writer& out) -> io::result
        {
#ifndef __cpp_lib_atomic_ref
            // don't allow updates while encoding
            std::lock_guard lock{mutex};
#endif

            std::size_t total = 0;

            for (std::size_t i = 0; i < buckets.size(); ++i)
            {
                auto res = io::write_all(
                    out,
                    name,
                    "_bucket{",
                    [this, i](io::writer& out) {
                        return encode_one_label(out,
                                                "le",
                                                std::isfinite(buckets[i]) ? std::to_string(buckets[i]) : "+Inf");
                    },
                    [this](io::writer& out) { return encode_all_labels(out); },
                    "} ",
                    [this, i](io::writer& out)
                    {
#ifdef __cpp_lib_atomic_ref
                        std::atomic_ref ref{bucket_values[i]};
                        auto            val = ref.load(std::memory_order_acquire);
#else
                        auto val = bucket_values[i];
#endif
                        return out.write(std::to_string(val));
                    },
                    '\n');
                total += res.count;
                if (res.err) return {.count = total, .err = res.err};
            }

            return {.count = total};
        },
        name,
        "_sum",
        [this](io::writer& out) { return encode_labels(out); },
        ' ',
        [this](io::writer& out)
        {
            auto sum_val = sum.load(std::memory_order_acquire);
            return out.write(std::to_string(sum_val));
        },
        '\n',
        name,
        "_count",
        [this](io::writer& out) { return encode_labels(out); },
        ' ',
        [this](io::writer& out)
        {
            auto count_val = count.load(std::memory_order_acquire);
            return out.write(std::to_string(count_val));
        },
        '\n');
}

void histogram::on_derive(histogram& child) { child.buckets = buckets; }

}
