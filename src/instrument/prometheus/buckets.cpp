#include "instrument/prometheus/buckets.hpp"

namespace net::instrument::prometheus
{

std::vector<double> default_buckets()
{
    // defaults are supposed to match other client libraries, so copying these from
    // the official Golang client:
    // https://github.com/prometheus/client_golang/blob/3583c1e1d085b75cab406c78b015562d45552b39/prometheus/histogram.go#L264C56-L264C56
    return {
        0.005,
        0.01,
        0.025,
        0.05,
        0.1,
        0.25,
        0.5,
        1.0,
        2.5,
        5.0,
        10.0,
        std::numeric_limits<double>::infinity(),
    };
}

std::vector<double> linear_buckets(double start, double width, std::size_t count)
{
    std::vector<double> buckets(count);

    auto next_upper_bound = start;

    for (std::size_t i = 0; i < count - 1; ++i)
    {
        buckets[i] = next_upper_bound;
        next_upper_bound += width;
    }

    buckets.back() = std::numeric_limits<double>::infinity();

    return buckets;
}

std::vector<double> exponential_buckets(double start, double factor, std::size_t count)
{
    std::vector<double> buckets(count);

    auto next_upper_bound = start;

    for (std::size_t i = 0; i < count - 1; ++i)
    {
        buckets[i] = next_upper_bound;
        next_upper_bound *= factor;
    }

    buckets.back() = std::numeric_limits<double>::infinity();

    return buckets;
}

std::vector<double> exponential_buckets_from_range(double min, double max, std::size_t count)
{
    auto factor = std::pow(max / min, 1.0 / static_cast<double>(count - 2));

    std::vector<double> buckets(count);

    for (std::size_t i = 0; i < count - 1; ++i)
    {
        buckets[i] = min * std::pow(factor, i);
    }

    buckets.back() = std::numeric_limits<double>::infinity();

    return buckets;
}

}
