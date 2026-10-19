#pragma once

#include <cstdint>
#include <system_error>
#include <type_traits>

namespace net::dns
{

// errc are the resolver's own errors, for what a DNS server answered (dns/message.hpp's codec reports bad_response, and
// maps RCODEs to the others). The system's resolver, getaddrinfo(), reports its own codes, in gai_category(): compare
// either with condition.
enum class errc : std::uint8_t
{
    // the name doesn't exist (NXDOMAIN), under any of the names searched.
    name_not_found = 1,
    // the name exists, but has no addresses of the family asked for.
    no_address = 2,
    // the servers failed (SERVFAIL), or gave nothing usable.
    server_failure = 3,
    // the servers refused to answer.
    refused = 4,
    // a reply was malformed, or matched no query.
    bad_response = 5,
};

[[nodiscard]] const std::error_category& category() noexcept;

[[nodiscard]] std::error_code make_error_code(errc err) noexcept;

// condition groups the resolvers' errors that mean the same to most callers, whichever reported them: errc's, and
// getaddrinfo()'s (in gai_category()).
enum class condition : std::uint8_t
{
    // the name has no addresses: it doesn't exist (errc::name_not_found, EAI_NONAME), or has none of the family asked
    // for (errc::no_address, EAI_NODATA where it's defined).
    not_found = 1,
    // the lookup failed, but might not if tried again later (errc::server_failure, EAI_AGAIN, EAI_FAIL).
    temporary_failure = 2,
};

[[nodiscard]] const std::error_category& condition_category() noexcept;

[[nodiscard]] std::error_condition make_error_condition(condition cond) noexcept;

}

namespace std
{

template<>
struct is_error_code_enum<net::dns::errc> : std::true_type
{};

template<>
struct is_error_condition_enum<net::dns::condition> : std::true_type
{};

}
