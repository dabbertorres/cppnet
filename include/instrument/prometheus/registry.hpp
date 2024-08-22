#pragma once

#include <concepts>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string_view>
#include <unordered_map>
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
#include "util/hash.hpp"
#include "util/string_map.hpp"

namespace std
{

template<typename T>
struct hash<std::vector<T>>
{
    constexpr std::size_t operator()(const std::vector<T>& value) const
    {
        std::size_t h = std::hash<std::size_t>{}(value.size());
        for (const auto& elem : value)
        {
            h = net::util::detail::hash_combine(h, std::hash<T>{}(elem));
        }

        return h;
    }
};

}

namespace net::instrument::prometheus
{

using metric = std::variant<counter, gauge, histogram>;

template<typename T, typename V, typename S = std::make_index_sequence<std::variant_size_v<V>>, std::size_t... Is>
concept variant_has = requires(V v, S)
// clang-format off
{
    (... || std::same_as<T, std::variant_alternative_t<Is, V>>);
};
// clang-format on

class registry
{
public:
    template<variant_has<metric> T>
    static T& register_metric(T&& m)
    {
        auto* r = get();

        std::lock_guard lock{r->mutex};
        return std::get<T>(
            r->get_metric(m.name, m.labels)
                .or_else([r, m = std::forward<T>(m)]()
                         { return std::make_optional(r->register_metric(m.name, m.labels, std::move(m))); })
                .value()
                .get());
    }

    template<variant_has<metric> T>
    static std::optional<std::reference_wrapper<T>> get_metric(std::string_view name, const metric_labels& labels)
    {
        auto* r = get();

        std::shared_lock lock{r->mutex};
        return r->get_metric(name, labels)
            .transform([](metric_ref val) -> std::reference_wrapper<T> { return std::ref(std::get<T>(val.get())); });
    }

    static coro::task<io::result> record(io::writer& writer);

private:
    using metric_ref       = std::reference_wrapper<metric>;
    using submetric_lookup = std::unordered_map<metric_labels, metric_ref>;
    using lookup_table     = util::string_map<submetric_lookup>;

    registry() = default;

    static registry* get();
    // NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
    static std::unique_ptr<registry> instance;
    static std::once_flag            initialized;
    // NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

    metric_ref                register_metric(std::string_view name, const metric_labels& labels, metric&& m);
    std::optional<metric_ref> get_metric(std::string_view name, const metric_labels& labels);
    coro::task<io::result>    record_all(io::writer& out) const;

    std::shared_mutex  mutex;
    std::deque<metric> metrics;

    lookup_table lookup;
};

}
