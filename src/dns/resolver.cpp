#include "dns/resolver.hpp"

#include <utility>

namespace net::dns
{

resolver::resolver(transport mode, std::vector<ip_addr> servers) noexcept
    : mode{mode}
    , servers{std::move(servers)}
{}

}
