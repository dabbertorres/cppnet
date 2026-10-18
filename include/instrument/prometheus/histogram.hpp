#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "buckets.hpp"
#include "metric.hpp"
#include "tracker.hpp"

namespace net::instrument::prometheus
{

struct histogram : public base_metric<histogram>
{
    static constexpr metric_type type() noexcept { return metric_type::histogram; }

    // buckets are the buckets' upper bounds, which must be increasing (or std::invalid_argument is thrown). A +Inf
    // bucket is added, if they don't end with one. The labels can't include "le", which the buckets use.
    explicit histogram(std::string&&         name,
                       std::string&&         help    = "",
                       metric_labels&&       labels  = {},
                       std::vector<double>&& buckets = default_buckets());

    // NOTE: copying or moving isn't safe while observing.
    histogram(const histogram& other);
    histogram& operator=(const histogram& other);

    histogram(histogram&& other) noexcept;
    histogram& operator=(histogram&& other) noexcept;

    ~histogram() = default;

    // observe counts v in the first bucket whose upper bound is at least v (NaN only in the +Inf bucket), and adds it
    // to the sum. It's safe to call from any number of threads at once.
    void observe(double v) noexcept;

    // buckets returns the buckets' upper bounds, ending with +Inf.
    [[nodiscard]] const std::vector<double>& buckets() const noexcept { return bounds; }

    // track returns a tracker, which observes how long it lived, in seconds.
    auto track() noexcept
    {
        return tracker{[this](std::chrono::duration<double> elapsed) { observe(elapsed.count()); }};
    }

private:
    friend struct base_metric<histogram>;

    void encode_own_samples(std::string& out) const;

    // NOTE: each bucket counts only the observations that belong in it, rather than also those in the buckets before
    //       it (as the output does), so an observation is one atomic increment, and the output adds them up: then its
    //       buckets never decrease, and the +Inf bucket always equals the count.
    std::vector<double> bounds;
    // NOLINTNEXTLINE(*-avoid-c-arrays): std::atomic can't be moved, so a std::vector can't hold them.
    std::unique_ptr<std::atomic<std::uint64_t>[]> counts;
    std::atomic<double>                           sum = 0.0;
};

}
