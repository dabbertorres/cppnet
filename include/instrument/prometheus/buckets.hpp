#pragma once

#include <cstddef>
#include <vector>

namespace net::instrument::prometheus
{

// Each returns count buckets' upper bounds, the last of which is +Inf (so count - 1 finite ones), or throws
// std::invalid_argument if they'd be invalid (e.g. not increasing).

// default_buckets are the other client libraries' default buckets: 5ms to 10s.
std::vector<double> default_buckets();

// linear_buckets start at start, each width more than the last.
std::vector<double> linear_buckets(double start, double width, std::size_t count);

// exponential_buckets start at start, each factor times the last.
std::vector<double> exponential_buckets(double start, double factor, std::size_t count);

// exponential_buckets_from_range go from min to max, each a constant factor times the last.
std::vector<double> exponential_buckets_from_range(double min, double max, std::size_t count);

}
