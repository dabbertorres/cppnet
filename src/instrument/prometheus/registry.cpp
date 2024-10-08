#include "instrument/prometheus/registry.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string_view>
#include <utility>
#include <variant>

#include "coro/task.hpp"
#include "instrument/prometheus/counter.hpp"
#include "instrument/prometheus/gauge.hpp"
#include "instrument/prometheus/histogram.hpp"
#include "instrument/prometheus/metric.hpp"
#include "io/io.hpp"
#include "io/writer.hpp"
#include "util/overloaded.hpp"

namespace net::instrument::prometheus
{

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
std::unique_ptr<registry> registry::instance;
std::once_flag            registry::initialized;
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

coro::task<io::result> registry::record(io::writer& writer)
{
    auto* r = get();

    std::shared_lock lock{r->mutex};
    return r->record_all(writer);
}

registry* registry::get()
{
    std::call_once(initialized, []() { instance = std::unique_ptr<registry>(new registry()); });

    return instance.get();
}

registry::metric_ref registry::register_metric(std::string_view name, const metric_labels& labels, metric&& m)
{
    auto found = get_metric(name, labels);
    if (found.has_value()) return found.value();

    auto& ref = metrics.emplace_back(std::move(m));
    lookup[name].emplace(labels, std::ref(ref));
    return ref;
}

std::optional<registry::metric_ref> registry::get_metric(std::string_view name, const metric_labels& labels)
{
    if (auto it = lookup.find(name); it != lookup.end())
    {
        if (auto sub_it = it->second.find(labels); sub_it != it->second.end())
        {
            return std::make_optional(sub_it->second);
        }
    }

    return std::nullopt;
}

coro::task<io::result> registry::record_all(io::writer& out) const
{
    std::size_t total = 0;

    for (const auto& m : metrics)
    {
        auto res = co_await std::visit(
            util::overloaded{
                [&](const counter& counter) { return counter.encode(out); },
                [&](const gauge& gauge) { return gauge.encode(out); },
                [&](const histogram& histogram) { return histogram.encode(out); },
            },
            m);

        total += res.count;

        if (res.err) co_return {.count = total, .err = res.err};
    }

    co_return {.count = total};
}

}
