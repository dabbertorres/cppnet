#pragma once

#include <cstdint>
#include <vector>

#include "ip_addr.hpp"

namespace net::dns
{

class resolver
{
public:
    enum class transport : std::uint8_t
    {
        udp,
        tcp,
    };

    resolver(transport mode, std::vector<ip_addr> servers) noexcept;

private:
    [[maybe_unused]] transport mode; // TODO: unused until the resolver is implemented.
    std::vector<ip_addr> servers;
    // TODO: cache
};

}
