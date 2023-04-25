#include <iostream>

#include "dns.hpp"

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::cerr << "Must provide a single hostname to resolve\n";
        return 1;
    }

    for (auto addr : net::dns_lookup(argv[1]))
    {
        std::cout << (addr.is_ipv4() ? "IPv4: " : "IPv6: ") << addr.to_string() << std::endl;
    }

    return 0;
}
