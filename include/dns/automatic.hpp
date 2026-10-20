#pragma once

#include <optional>
#include <string_view>

#include "dns/resolver.hpp"

// resolver::backend::automatic's choice, as Go's resolver makes it (its conf.go): internal, for the resolver and its
// tests, not for applications.
namespace net::dns::detail
{

// fixed_choice is the back end automatic chooses for every name, if the environment or the OS decide it (read when a
// resolver is made, as Go reads them once): NET_DNS's, if it's "system" or "native" (other values are ignored, as Go
// ignores the GODEBUG netdns values it doesn't know); otherwise the system's on an OS whose configuration the native
// back end doesn't read (all but Linux and the BSDs, and OpenBSD), or if LOCALDOMAIN is set (even if empty), or
// RES_OPTIONS or HOSTALIASES isn't empty (they change what the C library does). None: it's chosen per name, by
// choose().
[[nodiscard]] std::optional<resolver::backend> fixed_choice(const system_facts& facts);

// choose is the back end automatic chooses for name when fixed_choice() is none, from conf (Go's hostLookupOrder):
// the system's if resolv.conf has an unknown option, or if, going through nsswitch.conf's hosts sources in order, it
// meets:
// - an action that can't be read (an unclosed bracket, a criterion without '=');
// - files or dns with an action besides the defaults (each criterion its status's default, or "return" on the last
//   source, where it comes to the same);
// - myhostname, when name is the machine's own (facts.host_name's; any, if it's empty), a localhost name, _gateway or
//   _outbound (other names skip it);
// - an mdns source (mdns4_minimal, ...), when name ends in .local, or conf.mdns_allow (other names skip it);
// - any other source;
// or neither files nor dns. Otherwise (with no hosts sources too), the native.
[[nodiscard]] resolver::backend choose(const config& conf, std::string_view name, const system_facts& facts);

}
