#pragma once

#include <vector>

#include "ip_addr.hpp"

namespace net::dns
{

class resolver
{
public:
    enum class transport
    {
        udp,
        tcp,
    };

    resolver(transport mode, std::vector<ip_addr> servers) noexcept;

private:
    transport            mode;
    std::vector<ip_addr> servers;
    // TODO: cache
};

}
