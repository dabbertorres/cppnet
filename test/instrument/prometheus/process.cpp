#include "instrument/prometheus/process.hpp"

#include <array>
#include <exception> // IWYU pragma: keep
#include <string>
#include <string_view>

#include <catch.hpp>
#include <sys/socket.h>
#include <unistd.h>

#include <catch2/catch_test_macros.hpp>

#include "coro/sync_wait.hpp"
#include "instrument/prometheus/registry.hpp"
#include "io/string_writer.hpp"

using namespace std::string_literals;

using net::instrument::prometheus::register_process_metrics;
using net::instrument::prometheus::registry;

namespace
{

std::string record()
{
    net::io::string_writer<char> writer;
    auto                         res = test::sync_wait(registry::record(writer));
    REQUIRE_FALSE(res.err);
    return writer.build();
}

// sample returns the value of an unlabelled metric's sample, as written.
std::string sample(const std::string& text, std::string_view name)
{
    auto line = "\n" + std::string{name} + " ";
    auto at   = text.find(line);
    if (at == std::string::npos) return "";

    auto begin = at + line.size();
    return text.substr(begin, text.find('\n', begin) - begin);
}

}

// NOTE: one test, as the registry is global: nothing else registers process metrics, so they're only there once
//       this has.
TEST_CASE("process metrics are opt-in, and updated each record", "[instrument][prometheus][process]")
{
    CHECK(record().find("process_") == std::string::npos);

    register_process_metrics();
    register_process_metrics(); // again does nothing.

    auto before = record();

    for (auto name : {"process_cpu_seconds_total",
                      "process_open_fds",
                      "process_max_fds",
                      "process_virtual_memory_bytes",
                      "process_virtual_memory_max_bytes",
                      "process_resident_memory_bytes",
                      "process_heap_bytes",
                      "process_start_time_seconds",
                      "process_threads"})
    {
        CAPTURE(name);
        CHECK(before.find("# HELP "s + name + ' ') != std::string::npos);
        CHECK_FALSE(sample(before, name).empty());
    }

    CHECK(before.find("# TYPE process_cpu_seconds_total counter\n") != std::string::npos);
    CHECK(before.find("# TYPE process_open_fds gauge\n") != std::string::npos);

    // the next record sees two more open descriptors.
    std::array<int, 2> fds{};
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds.data()) == 0);
    auto after = record();
    ::close(fds[0]);
    ::close(fds[1]);

    CHECK(std::stoul(sample(after, "process_open_fds")) == std::stoul(sample(before, "process_open_fds")) + 2);
}
