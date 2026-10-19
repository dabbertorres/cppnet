#pragma once

#include <cstdint>

// tls::version, apart from the rest of TLS (tls/tls.hpp), so that code built without it (NET_HAS_TLS) can name one:
// e.g. server_request::tls_version, which is none then.
namespace net::tls
{

// version is a TLS protocol version, by its value on the wire (as Go's VersionTLS12 and VersionTLS13), so versions
// compare in the order they came; none (no TLS: e.g. a cleartext connection's) is before them all.
enum class version : std::uint16_t
{
    none  = 0,
    tls12 = 0x0303,
    tls13 = 0x0304,
};

}
