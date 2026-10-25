#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "coro/task.hpp"
#include "dns/config.hpp"
#include "io/scheduler.hpp"
#include "ip_addr.hpp"

// The native resolver's pieces (see resolver::backend::native): internal, for the resolver and its tests, not for
// applications.
namespace net::dns::detail
{

// max_cname_links is how many CNAME records a lookup follows, within an answer, from the name asked to the one with
// addresses.
inline constexpr std::size_t max_cname_links = 8;

// name_list is the names to ask for, for name, in order (Go's nameList): a name ending in a dot only as it is (without
// the dot); otherwise, under each of conf's search domains, and as it is, first if it has at least conf.ndots dots, or
// else last. Names over 253 bytes, with empty labels or labels over 63, or under .onion (RFC 7686), are left out.
[[nodiscard]] std::vector<std::string> name_list(const config& conf, std::string_view name);

// random_id is a query ID, from the OS's randomness (util::random_bytes(), taken a batch at a time per thread).
[[nodiscard]] std::uint16_t random_id() noexcept;

// sort_by_rfc6724 sorts addrs by RFC 6724's destination address selection (as Go's addrselect.go: rules 1, 2, 5, 6, 8
// and 9, the others needing what the OS doesn't say), each with sources[i], the source address the OS would use to
// reach it (none: it can't be reached). Equal ones keep their order. sources must be as long as addrs.
void sort_by_rfc6724(std::vector<ip_addr>& addrs, std::span<const std::optional<ip_addr>> sources);

// sort_by_rfc6724 sorts addrs with the source addresses the OS would use: found by connecting a UDP socket to each
// (which sends nothing), and asking for its address.
void sort_by_rfc6724(std::vector<ip_addr>& addrs);

// found_function is told of a family's addresses (sorted by RFC 6724) as soon as a server has answered with them, while
// the other family's question is still open.
using found_function = std::function<void(protocol family, std::vector<ip_addr> addrs)>;

// native_lookup looks host (lower-cased) up as conf says: in its hosts table, and with DNS queries to its
// nameservers (starting with first_server's, of their count), as the resolver's native back end does. It's ended by
// stop (failing with std::errc::operation_canceled), as by the scheduler stopping. Looking up both families, it tells
// found (if set) of the first's addresses, if they come in before the other's answer (as they're then the lookup's,
// whatever the other's are).
[[nodiscard]] coro::task<std::expected<std::vector<ip_addr>, std::error_code>>
native_lookup(io::scheduler*                scheduler,
              std::shared_ptr<const config> conf,
              std::string                   host,
              protocol                      proto,
              std::size_t                   first_server,
              std::stop_token               stop,
              found_function                found = {});

}
