#include "instrument/prometheus/metric.hpp"

namespace net::instrument::prometheus
{

using namespace std::chrono_literals;
using namespace std::string_view_literals;

std::string_view metric_type_string(metric_type type) noexcept
{
    switch (type)
    {
    case metric_type::counter: return "counter"sv;
    case metric_type::gauge: return "gauge"sv;
    case metric_type::histogram: return "histogram"sv;
    /* case metric_type::summary: return "summary"sv; */
    case metric_type::untyped: return "untyped"sv;
    }
}

}
