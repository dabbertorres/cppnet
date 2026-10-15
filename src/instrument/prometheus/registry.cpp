#include "instrument/prometheus/registry.hpp"

#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "coro/task.hpp"
#include "instrument/prometheus/metric.hpp"
#include "io/io.hpp"
#include "io/writer.hpp"

namespace net::instrument::prometheus
{

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
std::unique_ptr<registry> registry::instance;
std::once_flag            registry::initialized;
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

coro::task<io::result> registry::record(io::writer& writer)
{
    auto* r = get();

    // NOTE: the text is built under the lock, and only written once it's released: a lock can't be held across a
    //       suspension, as the task may then resume on another thread.
    std::string text;
    {
        std::shared_lock lock{r->mutex};

        for (const auto& name : r->family_order)
        {
            const auto& family = r->families.find(name)->second;

            // one HELP and TYPE for the family, and then each of its metrics' samples.
            std::visit([&](const auto& first) { first.encode_header(text); }, family.begin()->second.get());
            for (const auto& [_, m] : family)
            {
                std::visit([&](const auto& metric) { metric.encode_samples(text); }, m.get());
            }
        }
    }

    co_return co_await detail::write_text(writer, std::move(text));
}

registry* registry::get()
{
    std::call_once(initialized, []() { instance = std::unique_ptr<registry>(new registry()); });

    return instance.get();
}

registry::metric_ref registry::add(metric&& m)
{
    const auto& [name, help, labels] =
        std::visit([](const auto& metric) { return std::tie(metric.name, metric.help, metric.labels); },
                   std::as_const(m));

    auto it = families.find(name);
    if (it != families.end())
    {
        if (auto found = it->second.find(labels); found != it->second.end())
        {
            // NOTE: the type must match too, as register_metric returns it as m's type.
            if (found->second.get().index() != m.index())
            {
                throw std::invalid_argument{"metric \"" + name + "\" is already registered with another type"};
            }

            return found->second;
        }

        const auto& existing = it->second.begin()->second.get();
        if (existing.index() != m.index())
        {
            throw std::invalid_argument{"metric \"" + name + "\" is already registered with another type"};
        }

        if (std::visit([](const auto& metric) -> const std::string& { return metric.help; }, existing) != help)
        {
            throw std::invalid_argument{"metric \"" + name + "\" is already registered with another help"};
        }
    }

    // NOTE: copies of the name and labels, as m is moved.
    auto key_name   = name;
    auto key_labels = labels;

    auto& ref = metrics.emplace_back(std::move(m));

    if (it == families.end())
    {
        it = families.emplace(key_name, family{}).first;
        family_order.push_back(key_name);
    }

    it->second.emplace(std::move(key_labels), std::ref(ref));
    return ref;
}

std::optional<registry::metric_ref> registry::find(std::string_view name, const metric_labels& labels)
{
    if (auto it = families.find(name); it != families.end())
    {
        if (auto found = it->second.find(labels); found != it->second.end()) return found->second;
    }

    return std::nullopt;
}

}
