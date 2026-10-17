// http-get fetches a URL, and prints the response: e.g. `net-example-http-get http://example.com/`.

#include <cstddef>
#include <iostream>
#include <memory>
#include <string>

#include "coro/task.hpp"
#include "coro/thread_pool.hpp"
#include "http/client.hpp"
#include "http/http.hpp"
#include "io/scheduler.hpp"
#include "io/util.hpp"

namespace http = net::http;

namespace
{

// fetch GETs url, and prints the response, returning whether it could.
net::coro::task<bool> fetch(http::client& client, std::string url)
{
    auto resp = co_await client.get(url);
    if (!resp.has_value())
    {
        std::cerr << "request failed: " << resp.error().message() << '\n';
        co_return false;
    }

    std::cout << static_cast<int>(resp->status_code) << ' ' << http::reason_phrase(resp->status_code) << '\n';
    for (const auto& [name, values] : resp->headers)
    {
        for (const auto& value : values) std::cout << name << ": " << value << '\n';
    }
    std::cout << '\n';

    // the body is read after the head, until it ends (up to 10 MiB).
    constexpr std::size_t max_body = std::size_t{10} * 1024 * 1024;

    auto body = co_await net::io::read_all(*resp->body, max_body);
    if (!body.has_value())
    {
        std::cerr << "reading the body failed: " << body.error().message() << '\n';
        co_return false;
    }

    std::cout << *body << '\n';
    co_return true;
}

}

int main(int argc, char** argv) // NOLINT(misc-const-correctness): main's signature.
{
    if (argc != 2)
    {
        std::cerr << "usage: " << argv[0] << " <url>\n";
        return 1;
    }

    // the runtime: worker threads, and a scheduler that runs tasks on them and waits for their I/O.
    net::io::scheduler scheduler{std::make_shared<net::coro::thread_pool>()};
    http::client       client{&scheduler};

    // runs the task (and the scheduler, on this thread) until it's finished.
    return scheduler.run_to_completion(fetch(client, argv[1])) ? 0 : 1;
}
