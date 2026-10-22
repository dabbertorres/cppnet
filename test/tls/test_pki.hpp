#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

#include "tls/tls.hpp"
#include "tls/version.hpp"

// Certificates for the TLS tests (and the HTTP client's over TLS).
namespace test
{

// test_pki is certificate authorities, and certificates they issue, as PEM files in a temporary directory: made when
// the tests run, so that none expire.
struct test_pki
{
    std::filesystem::path dir;

    // the CAs the tests trust: one with an ECDSA key, and one with an RSA key.
    std::filesystem::path ca;

    // a certificate (and key) for localhost and 127.0.0.1, from the ECDSA CA: its key is ECDSA's, on P-256.
    std::filesystem::path localhost_chain;
    std::filesystem::path localhost_key;

    // the same, with an RSA key (2048 bits), from the RSA CA.
    std::filesystem::path rsa_localhost_chain;
    std::filesystem::path rsa_localhost_key;

    // one for another name, from the CA.
    std::filesystem::path other_name_chain;
    std::filesystem::path other_name_key;

    // one for several names (for an HTTP/2 connection's Origin Set): a.test, b.test, *.w.test and 127.0.0.1, from the
    // ECDSA CA.
    std::filesystem::path names_chain;
    std::filesystem::path names_key;

    // one for localhost, from a CA nothing trusts.
    std::filesystem::path untrusted_chain;
    std::filesystem::path untrusted_key;

    // the CA of clients' certificates (a server's client_ca_file: see tls::server_options::client_auth), named
    // "cppnet test client CA", with an ECDSA key, as all the clients' certificates have.
    std::filesystem::path client_ca;

    // the client CA and another (the unnamed client CA, below): a server that trusts these verifies the client
    // certificates client_ca's does, but its session tickets are bound to other CAs (see tls::server_options).
    std::filesystem::path client_ca_and_unnamed;

    // a client's certificate (and key) from the client CA, for clientAuth: its subject has what RFC 4514 writes
    // specially (client_subject is how), its serial number is client_serial, and it has a subject alternative name of
    // each kind (two DNS names, given out of order).
    std::filesystem::path client_chain;
    std::filesystem::path client_key;

    // what tls::certificate should read from it.
    std::vector<std::byte>                client_der;
    std::array<std::byte, 32>             client_sha256{};
    std::string                           client_subject;
    std::string                           client_serial;
    std::chrono::system_clock::time_point client_not_before;
    std::chrono::system_clock::time_point client_not_after;

    // client certificates from the client CA that don't verify: one that has expired, and one for servers only (its
    // extended key usage serverAuth, not clientAuth).
    std::filesystem::path expired_client_chain;
    std::filesystem::path expired_client_key;
    std::filesystem::path server_only_client_chain;
    std::filesystem::path server_only_client_key;

    // one from a CA with the client CA's name, but another key: a client sends it to a server that names the client
    // CA, which can't verify it.
    std::filesystem::path impostor_client_chain;
    std::filesystem::path impostor_client_key;

    // one from a CA with another name, which a server that trusts the client CA doesn't name: so a client doesn't
    // send it.
    std::filesystem::path unnamed_client_chain;
    std::filesystem::path unnamed_client_key;
};

// pki is the tests' certificates, made the first time it's called.
const test_pki& pki();

// trusting_client is a client context that trusts pki()'s CA, offering alpn.
net::tls::client_context trusting_client(std::vector<std::string> alpn = {});

// server_with is a server context with chain and key.
net::tls::server_context server_with(const std::filesystem::path& chain, const std::filesystem::path& key);

// client_auth_server is a server context with localhost's certificate, asking for clients' certificates as auth says
// (trusting the client CA, unless auth is none), and allowing TLS versions up to max_version.
net::tls::server_context client_auth_server(net::tls::client_auth auth,
                                            net::tls::version     max_version = net::tls::version::tls13);

// refusal is the alert a server sends refusing a client's certificate: bad_certificate for one that didn't verify
// (sent), or for none where one's required, TLS 1.3's certificate_required (or TLS 1.2's handshake_failure, as it has
// none).
net::tls::alert refusal(bool sent, net::tls::version version);

// client_with is a client context that trusts pki()'s CA, with chain and key as its certificate (none if empty),
// allowing TLS versions up to max_version, and offering alpn.
net::tls::client_context client_with(const std::filesystem::path& chain,
                                     const std::filesystem::path& key,
                                     net::tls::version            max_version = net::tls::version::tls13,
                                     std::vector<std::string>     alpn        = {});

}
