#include "instrument/prometheus/process.hpp"

#include <chrono>
#include <expected>
#include <functional>
#include <mutex>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "instrument/prometheus/counter.hpp"
#include "instrument/prometheus/gauge.hpp"
#include "instrument/prometheus/registry.hpp"
#include "instrument/system.hpp"

namespace
{


using net::instrument::prometheus::counter;
using net::instrument::prometheus::gauge;
using net::instrument::prometheus::registry;

using update = std::function<void()>;

// mirror registers a gauge of a statistic (if it's available), and returns how to update it.
template<typename Read>
void mirror(std::vector<update>& updates, std::string name, std::string help, Read read)
{
    auto first = read();
    if (!first) return;

    auto& g = registry::register_metric(gauge{std::move(name), std::move(help)});
    g.set(static_cast<double>(*first));

    updates.emplace_back(
        [&g, read]
        {
            if (auto v = read()) g.set(static_cast<double>(*v));
        });
}

}

namespace net::instrument::prometheus
{

void register_process_metrics()
{
    static std::once_flag registered;

    std::call_once(
        registered,
        []
        {
            std::vector<update> updates;

            if (auto cpu = system::process_cpu_seconds())
            {
                auto& c = registry::register_metric(
                    counter{"process_cpu_seconds_total", "Total user and system CPU time spent in seconds."});
                c.set(cpu->count());

                updates.emplace_back(
                    [&c]
                    {
                        if (auto v = system::process_cpu_seconds()) c.set(v->count());
                    });
            }

            mirror(updates, "process_open_fds", "Number of open file descriptors.", system::process_open_fds);
            mirror(updates, "process_max_fds", "Maximum number of open file descriptors.", system::process_max_fds);
            mirror(updates,
                   "process_virtual_memory_bytes",
                   "Virtual memory size in bytes.",
                   system::process_virtual_memory);
            mirror(updates,
                   "process_virtual_memory_max_bytes",
                   "Maximum amount of virtual memory available in bytes.",
                   system::process_virtual_memory_max);
            mirror(updates,
                   "process_resident_memory_bytes",
                   "Resident memory size in bytes.",
                   system::process_resident_memory);
            mirror(updates, "process_heap_bytes", "Process heap size in bytes.", system::process_heap);
            mirror(updates, "process_threads", "Number of OS threads in the process.", system::process_threads);

            // the start time doesn't change.
            if (auto start = system::process_start_time())
            {
                auto since_epoch = std::chrono::duration<double>{start->time_since_epoch()};
                registry::register_metric(
                    gauge{"process_start_time_seconds", "Start time of the process since unix epoch in seconds."})
                    .set(since_epoch.count());
            }

            registry::add_collector(
                [updates = std::move(updates)]
                {
                    for (const auto& u : updates) u();
                });
        });
}

}
