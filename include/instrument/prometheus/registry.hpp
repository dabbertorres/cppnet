#pragma once

#include <concepts>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "coro/task.hpp"
#include "counter.hpp"
#include "gauge.hpp"
#include "histogram.hpp"
#include "instrument/prometheus/metric.hpp"
#include "io/io.hpp"
#include "io/writer.hpp"
#include "util/string_map.hpp"

namespace net::instrument::prometheus
{

using metric = std::variant<counter, gauge, histogram>;

namespace detail
{

    template<typename T, typename Variant>
    struct is_alternative : std::false_type
    {};

    template<typename T, typename... Ts>
    struct is_alternative<T, std::variant<Ts...>> : std::bool_constant<(std::same_as<T, Ts> || ...)>
    {};

}

// variant_has is whether T is one of Variant's alternatives.
template<typename T, typename Variant>
concept variant_has = detail::is_alternative<T, Variant>::value;

// registry holds the metrics that are recorded (e.g. for Prometheus to scrape). Its metrics are never removed, so
// references to them stay valid.
//
// A metric's name is a metric family: metrics of the same name, but different labels, which must have the same type
// and help.
class registry
{
public:
    // register_metric registers m, unless a metric of the same name and labels already is, and returns the registered
    // metric. It throws std::invalid_argument if a metric of the same name has another type or help.
    template<variant_has<metric> T>
    static T& register_metric(T&& m)
    {
        auto* r = get();

        std::scoped_lock lock{r->mutex};
        return std::get<T>(r->add(metric{std::forward<T>(m)}).get());
    }

    // get_metric returns the metric of that type, name and labels, if it's registered.
    template<variant_has<metric> T>
    static std::optional<std::reference_wrapper<T>> get_metric(std::string_view name, const metric_labels& labels)
    {
        auto* r = get();

        std::shared_lock lock{r->mutex};
        auto             found = r->find(name, detail::normalize_labels(labels));
        if (!found.has_value()) return std::nullopt;

        T* m = std::get_if<T>(&found->get());
        if (m == nullptr) return std::nullopt;
        return std::ref(*m);
    }

    // add_collector adds a function that record calls each time, before writing the metrics, to update those whose
    // values are kept elsewhere (e.g. by the OS, see register_process_metrics).
    //
    // NOTE: collectors may run at the same time (if records do), and mustn't register metrics or collectors.
    static void add_collector(std::function<void()> collect);

    // record writes every metric, in Prometheus' text format, grouped by family (in the order they were registered).
    static coro::task<io::result> record(io::writer& writer);

private:
    using metric_ref = std::reference_wrapper<metric>;

    // NOTE: ordered by labels, so a family's metrics are written in the same order each time.
    using family = std::map<metric_labels, metric_ref>;

    registry() = default;

    static registry* get();
    // NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
    static std::unique_ptr<registry> instance;
    static std::once_flag            initialized;
    // NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

    metric_ref                add(metric&& m);
    std::optional<metric_ref> find(std::string_view name, const metric_labels& labels);

    std::shared_mutex  mutex;
    std::deque<metric> metrics;

    util::string_map<family> families;
    std::vector<std::string> family_order;

    std::vector<std::function<void()>> collectors;
};

}
