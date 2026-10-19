// ip-resolv prints a host name's addresses: e.g. `net-example-ip-resolv example.com`.

#include <chrono>
#include <expected>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include "coro/thread_pool.hpp"
#include "dns/resolver.hpp"
#include "io/scheduler.hpp"
#include "ip_addr.hpp"

int main(int argc, char** argv) // NOLINT(misc-const-correctness): main's signature.
{
    if (argc != 2)
    {
        std::cerr << "Must provide a single hostname to resolve\n";
        return 1;
    }

    net::io::scheduler scheduler{std::make_shared<net::coro::thread_pool>(1)};

    // gives up after 10 seconds.
    auto addrs = scheduler.run_to_completion(
        net::dns::default_resolver().resolve(&scheduler,
                                             argv[1],
                                             {.deadline = net::io::clock::now() + std::chrono::seconds{10}}));
    if (!addrs.has_value())
    {
        std::cerr << "failed to resolve " << argv[1] << ": " << addrs.error().message() << '\n';
        return 1;
    }

    for (const auto& addr : *addrs)
    {
        std::cout << (addr.is_ipv4() ? "IPv4: " : "IPv6: ") << addr.to_string() << '\n';
    }

    return 0;
}
