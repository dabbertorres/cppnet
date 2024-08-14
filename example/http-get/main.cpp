#include <array>
#include <exception>
#include <iostream>
#include <span>
#include <string_view>
#include <system_error>

#include <__expected/expected.h>

#include "http/client.hpp"
#include "http/request.hpp"
#include "http/response.hpp"
#include "io/scheduler.hpp"

#include "url.hpp"

namespace http = net::http;

int main(int argc, char** argv)
{
    using namespace std::chrono_literals;
    using namespace std::string_view_literals;

    if (argc != 2)
    {
        std::cerr << "Must provide a single url to GET.\n";
        return 1;
    }

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

    net::io::scheduler scheduler;
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
    std::expected<net::http::client_response, std::error_condition> get_result;

    try
    {
        get_result = client.send(req).operator co_await().await_resume();
    }
    catch (const std::exception& ex)
    {
        std::cerr << "Error sending request: " << ex.what() << "\n";
        return 1;
    }

    if (!get_result.has_value())
    {
        std::cerr << "Error sending request: " << get_result.error().message() << "\n";
        return 1;
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

        while (true)
        {
            auto res = resp.body->read(buf);
            if (res.count != 0)
            {
                std::cout << std::string_view{std::span{buf}.subspan(0, res.count)};
            }
            else
            {
                break;
            }

            if (res.err)
            {
                std::cerr << "error reading body: " << res.err.message() << '\n';
                return 1;
            }
        }

        std::cout << '\n';
    }

    return 0;
}
