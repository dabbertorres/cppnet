#include "dns.hpp"

#include <optional>
#include <string>
#include <string_view>

#include <netdb.h>
#include <sys/socket.h>

#include <netinet/in.h>

#include "coro/generator.hpp"
#include "exception.hpp"
#include "ip_addr.hpp"
#include "ipv4_addr.hpp"
#include "ipv6_addr.hpp"
#include "util/defer.hpp"

// Toggle to switch implementation of dns_lookup
#define USE_GETADDRINFO

#ifdef USE_GETADDRINFO

namespace net
{

coro::generator<ip_addr> dns_lookup(std::string_view hostname)
{
    // NOTE: not a designated initializer, as addrinfo's fields are in a different order on each platform.
    addrinfo hints{};
    hints.ai_flags  = AI_ADDRCONFIG;
    hints.ai_family = PF_UNSPEC;

    addrinfo* servinfo = nullptr;
    // NOTE: a copy, as getaddrinfo needs it null-terminated.
    auto sts = ::getaddrinfo(std::string{hostname}.c_str(), nullptr, &hints, &servinfo);
    if (sts != 0) throw_for_gai_error(sts);

    util::defer freeservinfo{[&] noexcept { freeaddrinfo(servinfo); }};

    // Skip consecutive duplicates (e.g. the same address for each socket type), which getaddrinfo appears to give in
    // order. Even if it didn't always, this is only a best effort.
    std::optional<ip_addr> prev;

    for (const auto* next = servinfo; next != nullptr; next = next->ai_next)
    {
        ip_addr addr;

        switch (next->ai_family)
        {
        case AF_INET: addr = ipv4_addr(reinterpret_cast<const sockaddr_in*>(next->ai_addr)->sin_addr.s_addr); break;
        case AF_INET6: addr = ipv6_addr(reinterpret_cast<const sockaddr_in6*>(next->ai_addr)->sin6_addr.s6_addr); break;
        default: continue;
        }

        if (addr == prev) continue;
        prev = addr;
        co_yield addr;
    }
}

}

#else

#    include "dns/resolver.hpp"

namespace net
{

coro::generator<ip_addr> dns_lookup(std::string_view hostname)
{
    // TODO
}

}

#endif
