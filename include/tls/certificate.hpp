#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

#include "ip_addr.hpp"

// tls::certificate, apart from the rest of TLS (tls/tls.hpp), so that code built without it (NET_HAS_TLS) can name one:
// e.g. server_request::peer_certificates, which is null then.
namespace net::tls
{

// certificate is an X.509 certificate a peer sent (see stream::peer_certificates()), as data: its DER encoding, and
// what's commonly checked, read from it (e.g. to authorize a client by its subject, a SPIFFE ID among its uris, or a
// pinned fingerprint). Anything else can be read from der, with an X.509 library.
struct certificate
{
    // the whole certificate, as it was sent.
    std::vector<std::byte> der{};

    // the subject's and issuer's distinguished names as RFC 4514 writes them, e.g. "CN=alice,O=Example": the last of
    // the certificate's relative distinguished names first. Attribute types RFC 4514 names (CN, L, ST, O, OU, C,
    // STREET, DC, UID) are written by name and the rest by OID, with their values' DER in hex ("2.5.4.5=#130131").
    std::string subject{};
    std::string issuer{};

    // the serial number, in upper case hex, two digits a byte, without leading zero bytes (e.g. "0A1B"; "00" for 0).
    std::string serial_number{};

    // when the certificate is valid: from not_before to not_after, inclusive. (A time beyond what the clock can hold is
    // its earliest or latest: e.g. RFC 5280's 9999-12-31 for no expiry, with libstdc++'s, which ends in 2262.)
    std::chrono::system_clock::time_point not_before{};
    std::chrono::system_clock::time_point not_after{};

    // the subject alternative name's entries, by kind: each kind sorted (not in the certificate's order).
    std::vector<std::string> dns_names{};
    std::vector<std::string> email_addresses{};
    std::vector<std::string> uris{};
    std::vector<ip_addr>     ip_addresses{};

    // the SHA-256 hash of der: e.g. for pinning a certificate.
    std::array<std::byte, 32> sha256{};
};

}
