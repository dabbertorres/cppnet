#include <array>
#include <condition_variable>
#include <expected>
#include <iostream>
#include <memory>
#include <mutex>
#include <span>
#include <string_view>
#include <system_error>

#include <spdlog/common.h>
#include <spdlog/spdlog.h>

#include "coro/task.hpp"
#include "coro/thread_pool.hpp"
#include "http/client.hpp"
#include "http/request.hpp"
#include "http/response.hpp"
#include "io/scheduler.hpp"
#include "url.hpp"

namespace http = net::http;

net::coro::task<>
run_request(http::client& client, http::client_request& req, std::condition_variable& is_done, bool& success);

int main(int argc, char** argv)
{
    using namespace std::chrono_literals;
    using namespace std::string_view_literals;

    if (argc != 2)
    {
        std::cerr << "Must provide a single url to GET.\n";
        return 1;
    }

    spdlog::set_level(spdlog::level::trace);

    auto parse_result = net::url::parse(argv[1]);
    if (parse_result.has_error())
    {
        auto err = parse_result.to_error();
        std::cerr << "Invalid url: unexpected character at " << err.index << " in "
                  << net::url_parse_state_to_string(err.failed_at) << "\n";
        return 1;
    }

    auto url = parse_result.to_value();
    if (url.scheme.empty()) url.scheme = "http";
    /* if (url.path.empty()) url.path = "/"; */
    if (url.port.empty())
    {
        if (url.scheme == "http") url.port = "80";
        else if (url.scheme == "https") url.port = "443";
    }

    std::cout << "fetching from:\n"
              << "scheme: " << url.scheme << '\n'
              << "userinfo: " << url.userinfo.username << '\n'
              << "host: " << url.host << '\n'
              << "port: " << url.port << '\n'
              << "path: " << url.path << '\n';

    auto workers = std::make_shared<net::coro::thread_pool>(1, spdlog::default_logger());

    net::io::scheduler scheduler{workers, spdlog::default_logger()};
    http::client       client{&scheduler};

    http::client_request req{
        .method  = http::request_method::GET,
        .version = {.major = 1, .minor = 1},
        .uri     = url,
        // clang-format off
        .headers = {
            {"User-Agent", {"cppnet/http/client"}},
            {"Accept", {"*/*"}},
        },
        // clang-format on
    };

    std::cout << "sending request...\n";

    std::mutex              is_done_mu;
    std::condition_variable is_done_cv;
    bool                    success = false;

    scheduler.schedule(run_request(client, req, is_done_cv, success));

    std::cout << "waiting for response...\n";
    std::unique_lock lock{is_done_mu};
    is_done_cv.wait(lock);

    std::cout << "done\n";

    return success ? 0 : 1;
}

net::coro::task<>
run_request(http::client& client, http::client_request& req, std::condition_variable& is_done, bool& success)
{
    auto get_result = co_await client.send(req);
    if (!get_result.has_value())
    {
        std::cerr << "Error sending request: " << get_result.error().message() << "\n";
        is_done.notify_one();
        co_return;
    }

    auto& resp = get_result.value();

    std::cout << http::status_text(resp.status_code) << '\n';

    for (const auto& [k, vals] : resp.headers)
    {
        std::cout << k << ":";

        for (const auto& v : vals)
        {
            std::cout << ' ' << v;
        }

        std::cout << '\n';
    }

    if (resp.body)
    {
        std::cout << '\n';

        std::array<char, 512> buf{};

        auto res = co_await resp.body->read(buf);
        if (res.err)
        {
            std::cerr << "error reading body: " << res.err.message() << '\n';
            is_done.notify_one();
            co_return;
        }

        if (res.count != 0)
        {
            std::cout << std::string_view{std::span{buf}.subspan(0, res.count)} << '\n';
        }
    }

    success = true;
    is_done.notify_one();
}
