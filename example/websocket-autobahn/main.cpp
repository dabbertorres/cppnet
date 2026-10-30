// websocket-autobahn runs the Autobahn TestSuite's fuzzingserver cases against cppnet's WebSocket client: it asks the
// server how many cases there are (/getCaseCount), then runs each (/runCase), echoing every message back whole until
// the WebSocket ends, and finally has the server write its reports (/updateReports), for the agent "cppnet".
//
// --url <ws://host:port> is the fuzzingserver's (ws://127.0.0.1:9001 by default). --agent <name> names the client in
// the reports. --cases <from>-<to> runs only those (numbered from 1, as the server counts them).
//
// e.g. with the fuzzingserver in Docker, its port published (a config whose url is ws://0.0.0.0:9001, in ./config):
//   docker run --rm -v "$PWD/config:/config" -v "$PWD/reports:/reports" -p 9001:9001
//       crossbario/autobahn-testsuite wstest -m fuzzingserver -s /config/fuzzingserver.json   (one command)
//   net-example-websocket-autobahn --url ws://127.0.0.1:9001

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <spdlog/spdlog.h>

#include "coro/task.hpp"
#include "coro/thread_pool.hpp"
#include "http/client.hpp"
#include "io/scheduler.hpp"
#include "websocket/websocket.hpp"

namespace http = net::http;
namespace ws   = net::websocket;

using net::coro::task;

namespace
{

// case_options are the cases' (section 9's largest messages are 16 MiB).
ws::connect_options case_options() { return {.connection = {.max_message = std::uint64_t{16} << 20}}; }

// read_text reads a WebSocket's first message (e.g. the case count), as text: empty if there's none.
task<std::string> read_text(const http::client& client, std::string url)
{
    auto conn = co_await ws::connect(client, url, case_options());
    if (!conn.has_value())
    {
        spdlog::error("{}: {}", url, conn.error().message());
        co_return std::string{};
    }

    std::string message;
    if (auto read = co_await conn->read_message(message); !read.has_value()) message.clear();

    // the server closes once it's sent it: its close is answered.
    std::string rest;
    while ((co_await conn->read_message(rest)).has_value())
    {}
    co_return message;
}

// run_case runs a case: it echoes each message whole, until the WebSocket ends (the server's close answered by the
// read that receives it, or a failure the client has told the server of).
task<> run_case(const http::client& client, std::string url)
{
    auto conn = co_await ws::connect(client, url, case_options());
    if (!conn.has_value())
    {
        spdlog::warn("{}: {}", url, conn.error().message());
        co_return;
    }

    std::string message;
    while (true)
    {
        const auto type = co_await conn->read_message(message);
        if (!type.has_value())
        {
            spdlog::debug("{}: ended: {}", url, type.error().message());
            co_return;
        }

        if (auto err = co_await conn->write_message(*type, message))
        {
            spdlog::debug("{}: write failed: {}", url, err.message());
            co_return;
        }
    }
}

task<int> run(net::io::scheduler* scheduler, std::string base, std::string agent, std::size_t from, std::size_t to)
{
    http::client client{scheduler};

    const auto  count_text = co_await read_text(client, base + "/getCaseCount");
    std::size_t count      = 0;
    if (std::from_chars(count_text.data(), count_text.data() + count_text.size(), count).ec != std::errc{}
        || count == 0)
    {
        spdlog::critical("no case count from {}", base);
        co_return 1;
    }

    to = std::min(to, count);
    spdlog::info("running cases {} to {} of {}", from, to, count);
    for (std::size_t i = from; i <= to; ++i)
    {
        co_await run_case(client, std::format("{}/runCase?case={}&agent={}", base, i, agent));
        if (i % 50 == 0) spdlog::info("{} cases run", i);
    }

    (void)co_await read_text(client, std::format("{}/updateReports?agent={}", base, agent));
    spdlog::info("reports updated");
    co_return 0;
}

}

int main(int argc, char** argv)
{
    using namespace std::string_view_literals;

    std::string base  = "ws://127.0.0.1:9001";
    std::string agent = "cppnet";
    std::size_t from  = 1;
    std::size_t to    = SIZE_MAX;

    const std::vector<std::string_view> args(argv + 1, argv + argc);
    for (std::size_t i = 0; i + 1 < args.size(); i += 2)
    {
        if (args[i] == "--url"sv)
        {
            base = args[i + 1];
        }
        else if (args[i] == "--agent"sv)
        {
            agent = args[i + 1];
        }
        else if (args[i] == "--cases"sv)
        {
            const auto range = args[i + 1];
            const auto dash  = range.find('-');
            const auto last  = dash == std::string_view::npos ? range : range.substr(dash + 1);
            std::from_chars(range.data(), range.data() + range.size(), from);
            std::from_chars(last.data(), last.data() + last.size(), to);
        }
    }

    if (args.size() % 2 != 0 || from == 0)
    {
        spdlog::critical("usage: {} [--url <ws://host:port>] [--agent <name>] [--cases <from>-<to>]", argv[0]);
        return 2;
    }

    net::io::scheduler scheduler{std::make_shared<net::coro::thread_pool>()};
    return scheduler.run_to_completion(run(&scheduler, base, agent, from, to));
}
