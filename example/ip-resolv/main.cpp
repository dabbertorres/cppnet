#include <iostream>
#include <string>
#include <string_view>

#include "coro/generator.hpp"
#include "dns.hpp"
#include "ip_addr.hpp"

int main(int argc, char** argv) // NOLINT(misc-const-correctness): main's signature.
{
    if (argc != 2)
    {
        std::cerr << "Must provide a single hostname to resolve\n";
        return 1;
    }

    for (auto addr : net::dns_lookup(argv[1]))
    {
        std::cout << (addr.is_ipv4() ? "IPv4: " : "IPv6: ") << addr.to_string() << '\n';
    }

    return 0;
}
