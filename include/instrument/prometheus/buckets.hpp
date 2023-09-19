#pragma once

#include <cstddef>
#include <vector>

namespace net::instrument::prometheus
{

std::vector<double> default_buckets();
std::vector<double> linear_buckets(double start, double width, std::size_t count);
std::vector<double> exponential_buckets(double start, double factor, std::size_t count);
std::vector<double> exponential_buckets_from_range(double min, double max, std::size_t count);

}
