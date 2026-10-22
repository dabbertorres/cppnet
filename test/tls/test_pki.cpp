#include "tls/test_pki.hpp"

// NOTE: before Botan's headers, some of which use <algorithm> without including it (see ports/botan).
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <botan/asn1_obj.h>
#include <botan/ber_dec.h>
#include <botan/bigint.h>
#include <botan/der_enc.h>
#include <botan/ec_group.h>
#include <botan/ecdsa.h>
#include <botan/hash.h>
#include <botan/ipv6_address.h>
#include <botan/pk_keys.h>
#include <botan/pkcs10.h>
#include <botan/pkcs8.h>
#include <botan/pkix_types.h>
#include <botan/pubkey.h>
#include <botan/rsa.h>
#include <botan/system_rng.h>
#include <botan/x509_ca.h>
#include <botan/x509_ext.h>
#include <botan/x509_obj.h>
#include <botan/x509cert.h>
#include <botan/x509self.h>
#include <catch2/catch_test_macros.hpp>

#include "tls/tls.hpp"

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

        // NOTE: RSA's keys are slow to make (more so in a debug build), so only two are.
        const auto make_key = [&](bool rsa) -> std::unique_ptr<Botan::Private_Key>
        {
            if (rsa) return std::make_unique<Botan::RSA_PrivateKey>(rng, 2048);
            return std::make_unique<Botan::ECDSA_PrivateKey>(rng, group);
        };

        const auto make_ca = [&](const std::string& name, bool rsa = false)
        {
            auto                     key = make_key(rsa);
            Botan::X509_Cert_Options opts;
            opts.common_name = name;
            opts.CA_key();
            auto cert = Botan::X509::create_self_signed_cert(opts, *key, "SHA-256", rng);
            return std::pair{cert, std::move(key)};
        };

        // issue writes a certificate for dns (and more_dns, and ip, if any), from ca, and its key (an RSA key if rsa,
        // or else ECDSA's).
        const auto issue = [&](const auto&                     ca,
                               const std::string&              dns,
                               const std::string&              ip,
                               const std::filesystem::path&    chain,
                               const std::filesystem::path&    key_path,
                               bool                            rsa      = false,
                               const std::vector<std::string>& more_dns = {})
        {
            const auto               key = make_key(rsa);
            Botan::X509_Cert_Options opts;
            opts.common_name = dns;
            opts.dns         = dns;
            opts.more_dns    = more_dns;
            opts.ip          = ip;

            const auto           request = Botan::X509::create_cert_req(opts, *key, "SHA-256", rng);
            const Botan::X509_CA authority{ca.first, *ca.second, "SHA-256", rng};

            const auto now = std::chrono::system_clock::now();
            const auto cert =
                authority.sign_request(request, rng, Botan::X509_Time{now - 1h}, Botan::X509_Time{now + 24h});

            write_file(chain, cert.PEM_encode());
            write_file(key_path, Botan::PKCS8::PEM_encode(*key));
        };

        // issue_client writes a client's certificate (and its key), from ca, for subject and names, for usage (an
        // extended key usage), valid from from until until, with serial as its serial number, returning it.
        // NOTE: made by hand (as X509_CA::sign_request() does), as the subject's DER is given, and the names (which
        //       X509_Cert_Options holds only an IP address of).
        const auto issue_client = [&](const auto&                           ca,
                                      const Botan::X509_DN&                 subject,
                                      const Botan::AlternativeName&         names,
                                      std::string_view                      usage,
                                      std::chrono::system_clock::time_point from,
                                      std::chrono::system_clock::time_point until,
                                      std::uint64_t                         serial,
                                      const std::filesystem::path&          chain,
                                      const std::filesystem::path&          key_path)
        {
            const auto               key = make_key(false);
            Botan::X509_Cert_Options opts;
            opts.common_name = "unused"; // the request's subject isn't the certificate's.
            opts.add_ex_constraint(usage);
            const auto request = Botan::X509::create_cert_req(opts, *key, "SHA-256", rng);

            auto extensions = Botan::X509_CA::choose_extensions(request, ca.first, "SHA-256");
            extensions.replace(std::make_unique<Botan::Cert_Extension::Subject_Alternative_Name>(names));

            const auto signer = Botan::X509_Object::choose_sig_format(*ca.second, rng, "SHA-256", "");
            auto       cert   = Botan::X509_CA::make_cert(*signer,
                                                          rng,
                                                          Botan::BigInt::from_u64(serial),
                                                          signer->algorithm_identifier(),
                                                          request.raw_public_key(),
                                                          Botan::X509_Time{from},
                                                          Botan::X509_Time{until},
                                                          ca.first.subject_dn(),
                                                          subject,
                                                          extensions);

            write_file(chain, cert.PEM_encode());
            write_file(key_path, Botan::PKCS8::PEM_encode(*key));
            return cert;
        };

        // name_of is the distinguished name of rdns (each a set of attributes: OID, value and string type), as they're
        // encoded. NOTE: by its DER, as an X509_DN made by adding attributes has one per RDN.
        using attribute    = std::tuple<std::string_view, std::string_view, Botan::ASN1_Type>;
        const auto name_of = [](const std::vector<std::vector<attribute>>& rdns)
        {
            std::vector<std::uint8_t> encoded;
            Botan::DER_Encoder        der{encoded};
            der.start_sequence();
            for (const auto& rdn : rdns)
            {
                der.start_set();
                for (const auto& [oid, value, type] : rdn)
                {
                    der.start_sequence()
                        .encode(Botan::OID::from_string(oid))
                        .encode(Botan::ASN1_String{value, type})
                        .end_cons();
                }
                der.end_cons();
            }
            der.end_cons();

            Botan::X509_DN dn;
            Botan::BER_Decoder{encoded}.decode(dn).verify_end();
            return dn;
        };

        const auto trusted     = make_ca("cppnet test CA");
        const auto trusted_rsa = make_ca("cppnet test RSA CA", true);
        const auto untrusted   = make_ca("cppnet untrusted CA");

        out.ca = out.dir / "ca.pem";
        write_file(out.ca, trusted.first.PEM_encode() + trusted_rsa.first.PEM_encode());

        out.localhost_chain     = out.dir / "localhost.pem";
        out.localhost_key       = out.dir / "localhost.key";
        out.rsa_localhost_chain = out.dir / "rsa-localhost.pem";
        out.rsa_localhost_key   = out.dir / "rsa-localhost.key";
        out.other_name_chain    = out.dir / "other.pem";
        out.other_name_key      = out.dir / "other.key";
        out.untrusted_chain     = out.dir / "untrusted.pem";
        out.untrusted_key       = out.dir / "untrusted.key";

        issue(trusted, "localhost", "127.0.0.1", out.localhost_chain, out.localhost_key);
        issue(trusted_rsa, "localhost", "127.0.0.1", out.rsa_localhost_chain, out.rsa_localhost_key, true);
        issue(trusted, "other.example", "", out.other_name_chain, out.other_name_key);
        issue(untrusted, "localhost", "127.0.0.1", out.untrusted_chain, out.untrusted_key);

        out.names_chain = out.dir / "names.pem";
        out.names_key   = out.dir / "names.key";
        issue(trusted, "a.test", "127.0.0.1", out.names_chain, out.names_key, false, {"b.test", "*.w.test"});

        const auto client_ca = make_ca("cppnet test client CA");
        const auto impostor  = make_ca("cppnet test client CA");
        const auto unnamed   = make_ca("cppnet unnamed client CA");

        out.client_ca = out.dir / "client-ca.pem";
        write_file(out.client_ca, client_ca.first.PEM_encode());
        out.client_ca_and_unnamed = out.dir / "client-ca-and-unnamed.pem";
        write_file(out.client_ca_and_unnamed, client_ca.first.PEM_encode() + unnamed.first.PEM_encode());

        constexpr auto utf8      = Botan::ASN1_Type::Utf8String;
        constexpr auto printable = Botan::ASN1_Type::PrintableString;

        // a country; a comma to escape; a control character; a multi-valued RDN (whose attributes DER sorts); a '#'
        // first and a space last; and an attribute type RFC 4514 doesn't name (serialNumber).
        const auto alice = name_of({
            {{"X520.Country", "NZ", printable}},
            {{"X520.Organization", "cppnet, tests", utf8}},
            {{"X520.Locality", "Wellington\x01", utf8}},
            {{"0.9.2342.19200300.100.1.1", "alice", utf8}, {"X520.OrganizationalUnit", "clients", utf8}},
            {{"X520.CommonName", "#alice ", utf8}},
            {{"X520.SerialNumber", "42", printable}},
        });
        out.client_subject =
            R"(2.5.4.5=#13023432,CN=\#alice\ ,OU=clients+UID=alice,L=Wellington\01,O=cppnet\, tests,C=NZ)";

        Botan::AlternativeName alice_names;
        alice_names.add_dns("b.alice.example");
        alice_names.add_dns("a.alice.example");
        alice_names.add_email("alice@example.com");
        alice_names.add_uri("spiffe://example.com/alice");
        alice_names.add_ipv4_address(0xC000'0201); // 192.0.2.1
        alice_names.add_ipv6_address(Botan::IPv6Address::from_string("2001:db8::1").value());

        const auto now        = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
        out.client_chain      = out.dir / "client.pem";
        out.client_key        = out.dir / "client.key";
        out.client_serial     = "0A1B2C";
        out.client_not_before = now - 1h;
        out.client_not_after  = now + 24h;

        const auto client = issue_client(client_ca,
                                         alice,
                                         alice_names,
                                         "PKIX.ClientAuth",
                                         out.client_not_before,
                                         out.client_not_after,
                                         0x0A'1B2C,
                                         out.client_chain,
                                         out.client_key);

        const auto der = client.BER_encode();
        out.client_der.resize(der.size());
        std::ranges::copy(std::as_bytes(std::span{der}), out.client_der.begin());

        const auto hash = Botan::HashFunction::create_or_throw("SHA-256");
        hash->update(der);
        const auto digest = hash->final_stdvec();
        std::ranges::copy(std::as_bytes(std::span{digest}), out.client_sha256.begin());

        // the other clients' certificates: bob's.
        const auto             bob = name_of({{{"X520.CommonName", "bob", utf8}}});
        Botan::AlternativeName bob_names;
        bob_names.add_dns("bob.example");

        const auto issue_bob = [&](const auto&                           ca,
                                   std::string_view                      usage,
                                   std::chrono::system_clock::time_point from,
                                   std::chrono::system_clock::time_point until,
                                   const std::string&                    name,
                                   std::filesystem::path&                chain,
                                   std::filesystem::path&                key)
        {
            chain = out.dir / (name + ".pem");
            key   = out.dir / (name + ".key");
            issue_client(ca, bob, bob_names, usage, from, until, 2, chain, key);
        };

        issue_bob(client_ca,
                  "PKIX.ClientAuth",
                  now - 48h,
                  now - 24h,
                  "expired-client",
                  out.expired_client_chain,
                  out.expired_client_key);
        issue_bob(client_ca,
                  "PKIX.ServerAuth",
                  now - 1h,
                  now + 24h,
                  "server-only-client",
                  out.server_only_client_chain,
                  out.server_only_client_key);
        issue_bob(impostor,
                  "PKIX.ClientAuth",
                  now - 1h,
                  now + 24h,
                  "impostor-client",
                  out.impostor_client_chain,
                  out.impostor_client_key);
        issue_bob(unnamed,
                  "PKIX.ClientAuth",
                  now - 1h,
                  now + 24h,
                  "unnamed-client",
                  out.unnamed_client_chain,
                  out.unnamed_client_key);
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

net::tls::server_context client_auth_server(net::tls::client_auth auth, net::tls::version max_version)
{
    std::optional<std::filesystem::path> client_ca;
    if (auth != net::tls::client_auth::none) client_ca = pki().client_ca;

    auto ctx = net::tls::server_context::make({.certificate_chain = pki().localhost_chain,
                                               .private_key       = pki().localhost_key,
                                               .max_version       = max_version,
                                               .client_auth       = auth,
                                               .client_ca_file    = client_ca});
    REQUIRE(ctx.has_value());
    return *ctx;
}

net::tls::alert refusal(bool sent, net::tls::version version)
{
    if (sent) return net::tls::alert::bad_certificate;
    return version == net::tls::version::tls13 ? net::tls::alert::certificate_required
                                               : net::tls::alert::handshake_failure;
}

net::tls::client_context client_with(const std::filesystem::path& chain,
                                     const std::filesystem::path& key,
                                     net::tls::version            max_version,
                                     std::vector<std::string>     alpn)
{
    net::tls::client_options options{.trusted_ca_file = pki().ca, .alpn = std::move(alpn), .max_version = max_version};
    if (!chain.empty())
    {
        options.certificate_chain = chain;
        options.private_key       = key;
    }

    auto ctx = net::tls::client_context::make(options);
    REQUIRE(ctx.has_value());
    return *ctx;
}

}
