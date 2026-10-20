#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "tls/tls.hpp"

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

    // one for localhost, from a CA nothing trusts.
    std::filesystem::path untrusted_chain;
    std::filesystem::path untrusted_key;
};

// pki is the tests' certificates, made the first time it's called.
const test_pki& pki();

// trusting_client is a client context that trusts pki()'s CA, offering alpn.
net::tls::client_context trusting_client(std::vector<std::string> alpn = {});

// server_with is a server context with chain and key.
net::tls::server_context server_with(const std::filesystem::path& chain, const std::filesystem::path& key);

}
