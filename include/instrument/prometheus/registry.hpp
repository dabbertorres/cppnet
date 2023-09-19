#pragma once

#include <concepts>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <variant>

#include "io/writer.hpp"

#include "counter.hpp"
#include "gauge.hpp"
#include "histogram.hpp"

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
        r->metrics.emplace_back(std::forward<T>(m));
        return std::get<T>(r->metrics.back());
    }

    static io::result record(io::writer& writer);

private:
    registry() = default;

    static registry* get();
    // NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
    static std::unique_ptr<registry> instance;
    static std::once_flag            initialized;
    // NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

    io::result record_all(io::writer& out) const;

    std::shared_mutex  mutex;
    std::deque<metric> metrics;
};

}
