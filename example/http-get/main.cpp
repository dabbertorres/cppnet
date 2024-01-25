#include <array>
#include <iostream>
#include <span>
#include <string_view>

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

    net::io::scheduler scheduler;
    http::client       client{&scheduler};

    http::client_request req{
        .method = http::request_method::GET,
        .uri    = url,
 // clang-format off
        .headers = {
            {"User-Agent", {"cppnet/http/client"}},
        },
  // clang-format on
    };

    auto get_result = client.send(req);

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
