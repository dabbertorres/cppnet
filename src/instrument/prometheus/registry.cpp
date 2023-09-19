#include "instrument/prometheus/registry.hpp"

#include <shared_mutex>
#include <string_view>
#include <type_traits>
#include <variant>

template<typename... Callables>
struct overloaded : Callables...
{
    using Callables::operator()...;
};

template<typename... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

namespace net::instrument::prometheus
{

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
std::unique_ptr<registry> registry::instance;
std::once_flag            registry::initialized;
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

io::result registry::record(io::writer& writer)
{
    auto* r = get();

    std::shared_lock lock{r->mutex};
    return r->record_all(writer);
}

registry* registry::get()
{
    std::call_once(initialized, [&]() { instance = std::unique_ptr<registry>(new registry()); });

    return instance.get();
}

io::result registry::record_all(io::writer& out) const
{
    std::size_t total = 0;

    for (const auto& m : metrics)
    {
        auto res = std::visit(
            overloaded{
                [&](const counter& counter) { return counter.encode(out); },
                [&](const gauge& gauge) { return gauge.encode(out); },
                [&](const histogram& histogram) { return histogram.encode(out); },
            },
            m);

        total += res.count;

        if (res.err) return {.count = total, .err = res.err};
    }

    return {.count = total};
}

}
