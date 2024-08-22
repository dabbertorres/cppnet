#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <shared_mutex>
#include <string>
#include <vector>
#include <version>

#include "buckets.hpp"
#include "coro/task.hpp"
#include "io/io.hpp"
#include "io/writer.hpp"
#include "metric.hpp"
#include "tracker.hpp"

namespace net::instrument::prometheus
{

struct histogram : public base_metric<histogram>
{
    static constexpr metric_type type() noexcept { return metric_type::histogram; }

    histogram(std::string&&         name,
              std::string&&         help    = "",
              metric_labels&&       labels  = {},
              std::vector<double>&& buckets = default_buckets());

    histogram(const histogram& other);
    histogram& operator=(const histogram& other);

    histogram(histogram&& other) noexcept;
    histogram& operator=(histogram&& other) noexcept;

    ~histogram() = default;

    void observe(double v) noexcept;

    auto track() noexcept
    {
        return tracker{[this](std::chrono::seconds seconds) { observe(static_cast<double>(seconds.count())); }};
    }

private:
    friend struct base_metric<histogram>;

    coro::task<io::result> encode_self(io::writer& out) const;
    void                   on_derive(histogram& child);

    std::vector<double>       buckets;
    std::vector<std::size_t>  bucket_values;
    std::atomic<std::size_t>  count = 0;
    std::atomic<double>       sum   = 0.0;
    mutable std::shared_mutex mutex;
};

}
