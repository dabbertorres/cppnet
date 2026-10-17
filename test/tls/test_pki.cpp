#include "tls/test_pki.hpp"

// NOTE: before Botan's headers, some of which use <algorithm> without including it (see ports/botan).
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <botan/ec_group.h>
#include <botan/ecdsa.h>
#include <botan/pkcs10.h>
#include <botan/pkcs8.h>
#include <botan/system_rng.h>
#include <botan/x509_ca.h>
#include <botan/x509cert.h>
#include <botan/x509self.h>
#include <catch2/catch_test_macros.hpp>

using namespace std::chrono_literals;

namespace test
{

namespace
{

    void write_file(const std::filesystem::path& path, std::string_view content)
    {
        std::ofstream out{path};
        out << content;
    }

}

const test_pki& pki()
{
    static const test_pki made = []
    {
        Botan::System_RNG rng;
        const auto        group = Botan::EC_Group::from_name("secp256r1");

        test_pki out;
        out.dir = std::filesystem::temp_directory_path() / ("cppnet-tls-" + std::to_string(std::random_device{}()));
        std::filesystem::create_directories(out.dir);

        const auto make_ca = [&](const std::string& name)
        {
            auto                     key = std::make_unique<Botan::ECDSA_PrivateKey>(rng, group);
            Botan::X509_Cert_Options opts;
            opts.common_name = name;
            opts.CA_key();
            auto cert = Botan::X509::create_self_signed_cert(opts, *key, "SHA-256", rng);
            return std::pair{cert, std::move(key)};
        };

        // issue writes a certificate for dns (and ip, if any), from ca, and its key.
        const auto issue = [&](const auto&                  ca,
                               const std::string&           dns,
                               const std::string&           ip,
                               const std::filesystem::path& chain,
                               const std::filesystem::path& key_path)
        {
            const Botan::ECDSA_PrivateKey key{rng, group};
            Botan::X509_Cert_Options      opts;
            opts.common_name = dns;
            opts.dns         = dns;
            opts.ip          = ip;

            const auto           request = Botan::X509::create_cert_req(opts, key, "SHA-256", rng);
            const Botan::X509_CA authority{ca.first, *ca.second, "SHA-256", rng};

            const auto now = std::chrono::system_clock::now();
            const auto cert =
                authority.sign_request(request, rng, Botan::X509_Time{now - 1h}, Botan::X509_Time{now + 24h});

            write_file(chain, cert.PEM_encode());
            write_file(key_path, Botan::PKCS8::PEM_encode(key));
        };

        const auto trusted   = make_ca("cppnet test CA");
        const auto untrusted = make_ca("cppnet untrusted CA");

        out.ca = out.dir / "ca.pem";
        write_file(out.ca, trusted.first.PEM_encode());

        out.localhost_chain  = out.dir / "localhost.pem";
        out.localhost_key    = out.dir / "localhost.key";
        out.other_name_chain = out.dir / "other.pem";
        out.other_name_key   = out.dir / "other.key";
        out.untrusted_chain  = out.dir / "untrusted.pem";
        out.untrusted_key    = out.dir / "untrusted.key";

        issue(trusted, "localhost", "127.0.0.1", out.localhost_chain, out.localhost_key);
        issue(trusted, "other.example", "", out.other_name_chain, out.other_name_key);
        issue(untrusted, "localhost", "127.0.0.1", out.untrusted_chain, out.untrusted_key);
        return out;
    }();

    return made;
}

net::tls::client_context trusting_client(std::vector<std::string> alpn)
{
    auto ctx = net::tls::client_context::make({.trusted_ca_file = pki().ca, .alpn = std::move(alpn)});
    REQUIRE(ctx.has_value());
    return *ctx;
}

net::tls::server_context server_with(const std::filesystem::path& chain, const std::filesystem::path& key)
{
    auto ctx = net::tls::server_context::make({.certificate_chain = chain, .private_key = key});
    REQUIRE(ctx.has_value());
    return *ctx;
}

}
