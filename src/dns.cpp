#include "dns.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <numbers>
#include <string_view>
#include <utility>

#include <netdb.h>

#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>

#include "coro/generator.hpp"
#include "util/defer.hpp"

#include "exception.hpp"
#include "ip_addr.hpp"
#include "ipv4_addr.hpp"
#include "ipv6_addr.hpp"

// Toggle to switch implementation of dns_lookup
#define USE_GETADDRINFO

#ifdef USE_GETADDRINFO

namespace
{

// neatly derive variants of the golden ratio for hash seeds regardless of architecture size

consteval long double pow(long double base, std::size_t exponent)
{
    auto result = base;
    for (auto i = 0ULL; i < exponent; ++i)
    {
        result *= base;
    }
    return result;
}

constexpr auto inv_phi = 1.L / std::numbers::phi_v<long double>;

consteval std::size_t hash_seed()
{
    auto seed = inv_phi * pow(2UL, sizeof(std::size_t) * 4 - 1);
    return static_cast<std::size_t>(seed);
}

}

namespace std
{

template<typename T, std::size_t N>
struct hash<T (&)[N]> // NOLINT(*-avoid-c-arrays)
{
    // NOLINTNEXTLINE(*-avoid-c-arrays)
    std::size_t operator()(const T (&data)[N]) const noexcept
    {
        // this is similar to how boost::hash_combine does it

        constexpr std::size_t seed = hash_seed();

        const std::hash<T> value_hasher;

        std::size_t result = 0;

        for (auto b : data)
        {
            auto next = value_hasher(b);
            result ^= next + seed + (result << 6) + (result >> 2);
        }

        return result;
    }
};

}

namespace net
{

coro::generator<ip_addr> dns_lookup(std::string_view hostname)
{
    addrinfo hints = {
        .ai_flags    = AI_ADDRCONFIG,
        .ai_family   = PF_UNSPEC,
        .ai_socktype = 0,
        .ai_protocol = 0,
    };

    addrinfo* servinfo = nullptr;
    auto      sts      = ::getaddrinfo(hostname.data(), nullptr, &hints, &servinfo);
    if (sts != 0) throw_for_gai_error(sts);

    util::defer freeservinfo{[&] noexcept { freeaddrinfo(servinfo); }};

    // Keep track of previous addresses to remove duplicates.
    // This assumes the addresses are given into us in a sorted
    // order, which appears to typically be the case.
    // Even if that turns out to not always be the case, this is fine,
    // since this is just a lightweight "best effort" attempt.
    std::size_t prev_hash = 0;

    for (auto* next = servinfo; next != nullptr; next = next->ai_next)
    {
        switch (next->ai_family)
        {
        case AF_INET:
        {
            auto addr = reinterpret_cast<sockaddr_in*>(next->ai_addr)->sin_addr.s_addr;
            auto hash = std::hash<decltype(addr)>{}(addr);

            if (hash != prev_hash) co_yield ipv4_addr(addr);
            prev_hash = hash;
            break;
        }

        case AF_INET6:
        {
            // NOLINTNEXTLINE(*-avoid-c-arrays)
            std::uint8_t(&addr)[16] = reinterpret_cast<sockaddr_in6*>(next->ai_addr)->sin6_addr.s6_addr;
            auto hash               = std::hash<decltype(addr)>{}(addr);

            if (hash != prev_hash) co_yield ipv6_addr(addr);
            prev_hash = hash;
            break;
        }

        default:
            [[unlikely]]
#    ifdef __cpp_lib_unreachable
            std::unreachable()
#    endif
                ;
        }
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
