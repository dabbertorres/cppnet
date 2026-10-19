#include "dns.hpp"

#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <netdb.h>
#include <sys/socket.h>

#include "exception.hpp"
#include "ip_addr.hpp"
#include "sockaddr.hpp"
#include "util/defer.hpp"

namespace net
{

std::expected<std::vector<ip_addr>, std::error_code> dns_lookup(std::string_view hostname)
{
    // NOTE: not a designated initializer, as addrinfo's fields are in a different order on each platform.
    addrinfo hints{};
    hints.ai_flags  = AI_ADDRCONFIG;
    hints.ai_family = PF_UNSPEC;

    addrinfo* servinfo = nullptr;
    // NOTE: a copy, as getaddrinfo needs it null-terminated.
    auto sts = ::getaddrinfo(std::string{hostname}.c_str(), nullptr, &hints, &servinfo);
    if (sts != 0) return std::unexpected(make_gai_error_code(sts));

    util::defer freeservinfo{[&] noexcept { freeaddrinfo(servinfo); }};

    // Skip consecutive duplicates (e.g. the same address for each socket type), which getaddrinfo appears to give in
    // order. Even if it didn't always, this is only a best effort.
    std::optional<ip_addr> prev;
    std::vector<ip_addr>   addrs;

    for (const auto* next = servinfo; next != nullptr; next = next->ai_next)
    {
        auto found = detail::from_sockaddr(next->ai_addr, next->ai_addrlen);
        if (!found.has_value()) continue;

        const auto addr = found->addr;
        if (addr == prev) continue;
        prev = addr;
        addrs.push_back(addr);
    }

    return addrs;
}

}
