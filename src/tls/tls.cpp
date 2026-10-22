#include "tls/tls.hpp"

// NOTE: before Botan's headers, some of which use <algorithm> without including it (see ports/botan).
#include <algorithm>
#include <array>
#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <ratio>
#include <set>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <sys/socket.h>

#include <botan/asn1_obj.h>
#include <botan/ber_dec.h>
#include <botan/certstor.h>
#include <botan/certstor_system.h>
#include <botan/credentials_manager.h>
#include <botan/data_src.h>
#include <botan/exceptn.h>
#include <botan/hash.h>
#include <botan/ipv6_address.h>
#include <botan/mac.h>
#include <botan/mem_ops.h>
#include <botan/ocsp.h>
#include <botan/pk_keys.h>
#include <botan/pkcs8.h>
#include <botan/pkix_enums.h>
#include <botan/pkix_types.h>
#include <botan/rng.h>
#include <botan/strong_type.h>
#include <botan/symkey.h>
#include <botan/system_rng.h>
#include <botan/tls_alert.h>
#include <botan/tls_algos.h>
#include <botan/tls_callbacks.h>
#include <botan/tls_channel.h>
#include <botan/tls_ciphersuite.h>
#include <botan/tls_client.h>
#include <botan/tls_exceptn.h>
#include <botan/tls_handshake_msg.h>
#include <botan/tls_magic.h>
#include <botan/tls_policy.h>
#include <botan/tls_server.h>
#include <botan/tls_server_info.h>
#include <botan/tls_session.h>
#include <botan/tls_session_id.h>
#include <botan/tls_session_manager.h>
#include <botan/tls_session_manager_memory.h>
#include <botan/tls_session_manager_noop.h>
#include <botan/tls_version.h>
#include <botan/x509cert.h>
#include <botan/x509path.h>

#include "coro/notifier.hpp"
#include "coro/task.hpp"
#include "encoding/hex.hpp"
#include "io/io.hpp"
#include "io/scheduler.hpp"
#include "io/writer.hpp"
#include "ip_addr.hpp"
#include "ipv4_addr.hpp"
#include "ipv6_addr.hpp"
#include "sockaddr.hpp"
#include "tcp.hpp"
#include "tls/certificate.hpp"
#include "util/defer.hpp"

namespace
{

std::string alert_name(int code) { return Botan::TLS::Alert(static_cast<Botan::TLS::Alert::Type>(code)).type_string(); }

class tls_category_impl final : public std::error_category
{
public:
    [[nodiscard]] const char* name() const noexcept override { return "tls"; }

    [[nodiscard]] std::string message(int code) const override
    {
        switch (static_cast<net::tls::errc>(code))
        {
        case net::tls::errc::stream_truncated: return "the peer closed the connection without close_notify";
        case net::tls::errc::protocol_error: return "the peer isn't speaking TLS (or broke its protocol)";
        }

        if (code >= 0 && code <= 255) return alert_name(code) + " (from the peer)";
        if (code >= net::tls::sent_alert_base && code <= net::tls::sent_alert_base + 255)
            return alert_name(code - net::tls::sent_alert_base) + " (sent to the peer)";
        return "unknown TLS error";
    }

    // a truncated stream still ended, as io::status_condition::closed groups.
    [[nodiscard]] std::error_condition default_error_condition(int code) const noexcept override
    {
        if (static_cast<net::tls::errc>(code) == net::tls::errc::stream_truncated)
            return make_error_condition(net::io::status_condition::closed);
        return {code, *this};
    }
};

class verification_category_impl final : public std::error_category
{
public:
    [[nodiscard]] const char* name() const noexcept override { return "tls::verification"; }

    [[nodiscard]] std::string message(int code) const override
    {
        return std::string{"the peer's certificate couldn't be verified: "}
             + Botan::to_string(static_cast<Botan::Certificate_Status_Code>(code));
    }

    // for which bad_certificate was sent.
    [[nodiscard]] std::error_condition default_error_condition(int /*code*/) const noexcept override
    {
        return make_error_condition(net::tls::alert::bad_certificate);
    }
};

class alert_category_impl final : public std::error_category
{
public:
    [[nodiscard]] const char* name() const noexcept override { return "tls::alert"; }

    [[nodiscard]] std::string message(int code) const override { return alert_name(code); }

    // an alert matches whichever side sent it. (Verification failures match bad_certificate by their
    // default_error_condition().)
    [[nodiscard]] bool equivalent(const std::error_code& code, int condition) const noexcept override
    {
        if (code.category() != net::tls::category()) return false;

        const auto value = code.value();
        if (value >= net::tls::sent_alert_base) return value - net::tls::sent_alert_base == condition;
        return value == condition;
    }
};

const verification_category_impl the_verification_category;
const alert_category_impl        the_alert_category;

// peer_alert is the error for an alert the peer sent.
std::error_code peer_alert(Botan::TLS::Alert::Type type) noexcept
{
    return {static_cast<int>(type), net::tls::category()};
}

// sent_alert is the error for an alert sent to the peer.
std::error_code sent_alert(Botan::TLS::Alert::Type type) noexcept
{
    return {net::tls::sent_alert_base + static_cast<int>(type), net::tls::category()};
}

// read_file reads a whole file, failing with the error opening it (e.g. no_such_file_or_directory).
std::expected<std::unique_ptr<Botan::DataSource_Stream>, std::error_code> open_file(const std::filesystem::path& path)
{
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec))
        return std::unexpected(ec ? ec : std::make_error_code(std::errc::no_such_file_or_directory));

    try
    {
        return std::make_unique<Botan::DataSource_Stream>(path.string());
    }
    catch (const std::exception&)
    {
        return std::unexpected(std::make_error_code(std::errc::io_error));
    }
}

// load_certificates reads every certificate in a PEM file, failing if there are none.
std::expected<std::vector<Botan::X509_Certificate>, std::error_code>
load_certificates(const std::filesystem::path& path)
{
    auto source = open_file(path);
    if (!source.has_value()) return std::unexpected(source.error());

    std::vector<Botan::X509_Certificate> certs;
    try
    {
        while (!(*source)->end_of_data()) certs.emplace_back(**source);
    }
    catch (const std::exception&)
    {
        // e.g. trailing text after the last certificate, which ends them.
        if (certs.empty()) return std::unexpected(std::make_error_code(std::errc::invalid_argument));
    }

    if (certs.empty()) return std::unexpected(std::make_error_code(std::errc::invalid_argument));
    return certs;
}

// groups are the key exchange groups the policy offers: Go's (X25519, P-256, P-384 and P-521), and the hybrid
// post-quantum ones Botan has (X25519MLKEM768, which Go and browsers offer, and P-256's and P-384's with ML-KEM), in
// Botan's order of preference. Botan's default policy also offers X448 and the brainpool curves, which cppnet's Botan
// isn't built with (see ports/botan), and finite field Diffie-Hellman, which Go doesn't offer either: so they're left
// out whichever Botan cppnet's built against.
constexpr std::array groups{
    Botan::TLS::Group_Params_Code::X25519,
    Botan::TLS::Group_Params_Code::SECP256R1,
    Botan::TLS::Group_Params_Code::HYBRID_X25519_ML_KEM_768,
    Botan::TLS::Group_Params_Code::HYBRID_SECP256R1_ML_KEM_768,
    Botan::TLS::Group_Params_Code::HYBRID_SECP384R1_ML_KEM_1024,
    Botan::TLS::Group_Params_Code::SECP384R1,
    Botan::TLS::Group_Params_Code::SECP521R1,
};

// policy is Botan's default TLS policy, except that it doesn't require revocation information (OCSP or CRLs) for the
// peer's certificates: many servers have none to give (e.g. Let's Encrypt's, which stopped OCSP in 2025), and neither
// browsers, Go, nor OpenSSL require it by default.
//
// Nor does it require the Extended Master Secret extension of TLS 1.2 peers (as Botan's does, but browsers, Go and
// OpenSSL don't), unless asked to.
//
// It allows the TLS versions from min to max. (Botan's TLS channels follow allow_tls12() and allow_tls13(): a client
// offers the latest allowed, and a server agrees on the latest the client offers that it allows, or fails the handshake
// with protocol_version. DTLS isn't used.)
//
// It asks for clients' certificates, and requires one, as client_auth says (a server's).
//
// Its sessions' tickets are good for ticket_lifetime (a server's: Botan's default, a day, for a client's).
//
// It offers only the key exchange groups above (those of them the Botan it's built against has), and so only ECDHE in
// TLS 1.2, not finite field DHE. Botan's default ciphers are already only AEADs (AES-GCM and ChaCha20-Poly1305), and
// its signatures only ECDSA and RSA, with SHA-256 to SHA-512.
//
// A client's first ClientHello (TLS 1.3) has key shares for two of them, as Go's and Chrome's do: the first hybrid
// post-quantum group (X25519MLKEM768) and the first elliptic curve one (X25519). Botan's default sends only the latter,
// so a server that prefers the hybrid (Botan's, so cppnet's, does whenever the client supports one; Cloudflare's too)
// asks for its share with a HelloRetryRequest, a round trip more.
class policy final : public Botan::TLS::Policy
{
public:
    policy(bool                            require_ems,
           net::tls::version               min,
           net::tls::version               max,
           net::tls::client_auth           auth            = net::tls::client_auth::none,
           net::tls::detail::policy_limits limits          = {},
           std::chrono::seconds            ticket_lifetime = std::chrono::days{1}) noexcept
        : require_ems{require_ems}
        , min{min}
        , max{max}
        , auth{auth}
        , limits{std::move(limits)}
        , ticket_lifetime{ticket_lifetime}
    {}

    [[nodiscard]] bool require_cert_revocation_info() const override { return false; }
    [[nodiscard]] bool require_extended_master_secret() const override { return require_ems; }

    // NOTE: a server with client CAs (see credentials) asks anyway: these say so explicitly, and whether to require a
    //       certificate (failing a handshake without one with certificate_required, or TLS 1.2's handshake_failure).
    [[nodiscard]] bool request_client_certificate_authentication() const override
    {
        return auth != net::tls::client_auth::none;
    }
    [[nodiscard]] bool require_client_certificate_authentication() const override
    {
        return auth == net::tls::client_auth::required;
    }

    [[nodiscard]] bool allow_tls12() const override { return allows(net::tls::version::tls12); }
    [[nodiscard]] bool allow_tls13() const override { return allows(net::tls::version::tls13); }

    [[nodiscard]] std::vector<Botan::TLS::Group_Params> key_exchange_groups() const override
    {
        std::vector<Botan::TLS::Group_Params> out;
        for (const Botan::TLS::Group_Params group : groups)
        {
            if (group.is_available() && within(limits.groups, group.wire_code())) out.push_back(group);
        }
        return out;
    }

    // NOTE: Botan sends the shares in the order of key_exchange_groups() (as RFC 8446 4.2.8 requires), whatever the
    //       order here; and leaves out those that aren't among them.
    [[nodiscard]] std::vector<Botan::TLS::Group_Params> key_exchange_groups_to_offer() const override
    {
        std::vector<Botan::TLS::Group_Params> out;
        if (!limits.key_shares.empty())
        {
            for (const std::uint16_t code : limits.key_shares) out.emplace_back(code);
            return out;
        }

        // the first hybrid and the first elliptic curve group of those allowed (so with the groups narrowed, e.g. to
        // one, only those there are); if there's neither (no groups at all), Botan's choice, which fails.
        const auto allowed = key_exchange_groups();
        if (const auto hybrid = std::ranges::find_if(allowed, &Botan::TLS::Group_Params::is_pqc_hybrid);
            hybrid != allowed.end())
            out.push_back(*hybrid);
        if (const auto ecc = std::ranges::find_if(allowed, &Botan::TLS::Group_Params::is_pure_ecc_group);
            ecc != allowed.end())
            out.push_back(*ecc);
        if (out.empty()) return Botan::TLS::Policy::key_exchange_groups_to_offer();
        return out;
    }

    [[nodiscard]] std::vector<std::string> allowed_key_exchange_methods() const override { return {"ECDH"}; }

    [[nodiscard]] std::chrono::seconds session_ticket_lifetime() const override { return ticket_lifetime; }

    [[nodiscard]] bool acceptable_ciphersuite(const Botan::TLS::Ciphersuite& suite) const override
    {
        return Botan::TLS::Policy::acceptable_ciphersuite(suite)
            && within(limits.cipher_suites, suite.ciphersuite_code());
    }

private:
    [[nodiscard]] bool allows(net::tls::version v) const noexcept { return min <= v && v <= max; }

    // within is whether code is among limit's, if it has any.
    [[nodiscard]] static bool within(const std::vector<std::uint16_t>& limit, std::uint16_t code) noexcept
    {
        return limit.empty() || std::ranges::contains(limit, code);
    }

    bool                            require_ems;
    net::tls::version               min;
    net::tls::version               max;
    net::tls::client_auth           auth;
    net::tls::detail::policy_limits limits;
    std::chrono::seconds            ticket_lifetime;
};

// valid_versions is whether min to max are versions there are, in order.
bool valid_versions(net::tls::version min, net::tls::version max) noexcept
{
    const auto known = [](net::tls::version v)
    { return v == net::tls::version::tls12 || v == net::tls::version::tls13; };
    return known(min) && known(max) && min <= max;
}

// credentials gives one side of connections (a client's, or a server's) the certificate authorities it trusts for the
// peer's certificates, and its own certificate chain and key, if it has them. Botan names the side by the type it asks
// for: "tls-client" for a client's (verifying servers' certificates, and the certificate to send a server that asks for
// one), "tls-server" for a server's (verifying clients' certificates, if it asks for them, and its own certificate).
class credentials final : public Botan::Credentials_Manager
{
public:
    // side is "tls-client" or "tls-server"; trusted the certificate authorities it trusts (for a server, those for its
    // clients' certificates: null if it doesn't ask for them, so it doesn't); chain and key its own, if any.
    credentials(std::string                               side,
                std::unique_ptr<Botan::Certificate_Store> trusted,
                std::vector<Botan::X509_Certificate>      chain = {},
                std::shared_ptr<Botan::Private_Key>       key   = nullptr) noexcept
        : side{std::move(side)}
        , trusted{std::move(trusted)}
        , chain{std::move(chain)}
        , key{std::move(key)}
    {}

    // NOTE: a server asks for clients' certificates when this gives any CAs (naming them in its CertificateRequest).
    std::vector<Botan::Certificate_Store*> trusted_certificate_authorities(const std::string& type,
                                                                           const std::string& /*context*/) override
    {
        if (type == side && trusted) return {trusted.get()};
        return {};
    }

    std::vector<Botan::X509_Certificate> find_cert_chain(const std::vector<std::string>& cert_key_types,
                                                         const std::vector<Botan::AlgorithmIdentifier>& /*schemes*/,
                                                         const std::vector<Botan::X509_DN>& acceptable_CAs,
                                                         const std::string&                 type,
                                                         const std::string& /*context*/) override
    {
        if (type != side || chain.empty()) return {};

        // the peer must accept the key's kind (e.g. ECDSA, RSA).
        if (!cert_key_types.empty() && std::ranges::find(cert_key_types, key->algo_name()) == cert_key_types.end())
            return {};

        // a client's certificate must be issued by one of the CAs the server names, if it names any (as Go's
        // SupportsCertificate checks): a server would refuse another, where it may accept none. (A server's isn't
        // held to the CAs a client names, as Go's isn't: it has only the one.)
        const auto named = [&acceptable_CAs](const Botan::X509_Certificate& cert)
        { return std::ranges::contains(acceptable_CAs, cert.issuer_dn()); };
        if (side == "tls-client" && !acceptable_CAs.empty() && !std::ranges::any_of(chain, named)) return {};

        return chain;
    }

    std::shared_ptr<Botan::Private_Key> private_key_for(const Botan::X509_Certificate& cert,
                                                        const std::string&             type,
                                                        const std::string& /*context*/) override
    {
        return type == side && !chain.empty() && cert == chain.front() ? key : nullptr;
    }

private:
    std::string                               side;
    std::unique_ptr<Botan::Certificate_Store> trusted;
    std::vector<Botan::X509_Certificate>      chain;
    std::shared_ptr<Botan::Private_Key>       key;
};

// identity is a certificate chain and its leaf's private key: a side's own.
struct identity
{
    std::vector<Botan::X509_Certificate> chain;
    std::shared_ptr<Botan::Private_Key>  key;
};

// load_identity reads a certificate chain and its private key, failing with the error opening either, or
// invalid_argument if they aren't what they should be (e.g. the key isn't PKCS #8), or the key isn't the certificate's.
std::expected<identity, std::error_code> load_identity(const std::filesystem::path& chain_path,
                                                       const std::filesystem::path& key_path)
{
    auto chain = load_certificates(chain_path);
    if (!chain.has_value()) return std::unexpected(chain.error());

    auto key_source = open_file(key_path);
    if (!key_source.has_value()) return std::unexpected(key_source.error());

    try
    {
        std::shared_ptr<Botan::Private_Key> key = Botan::PKCS8::load_key(**key_source);
        if (!key) return std::unexpected(std::make_error_code(std::errc::invalid_argument));

        // the certificate must be the key's: otherwise every handshake would fail, saying only that it had.
        // NOTE: both encoded by Botan, so a key encoded differently in the certificate (e.g. a compressed point)
        //       still matches.
        if (chain->front().subject_public_key()->subject_public_key() != key->subject_public_key())
            return std::unexpected(std::make_error_code(std::errc::invalid_argument));

        return identity{.chain = std::move(*chain), .key = std::move(key)};
    }
    catch (const std::exception&)
    {
        // e.g. the key isn't PKCS #8.
        return std::unexpected(std::make_error_code(std::errc::invalid_argument));
    }
}

// store_of is a store of certificate authorities.
std::unique_ptr<Botan::Certificate_Store> store_of(const std::vector<Botan::X509_Certificate>& certs)
{
    auto store = std::make_unique<Botan::Certificate_Store_In_Memory>();
    for (const auto& cert : certs) store->add_certificate(cert);
    return store;
}

// load_store reads the certificate authorities in a PEM file into a store, failing as load_certificates does.
std::expected<std::unique_ptr<Botan::Certificate_Store>, std::error_code> load_store(const std::filesystem::path& path)
{
    auto certs = load_certificates(path);
    if (!certs.has_value()) return std::unexpected(certs.error());
    return store_of(*certs);
}

// rfc4514_name is RFC 4514's name for an attribute type (section 3), by its OID: empty if it has none.
std::string_view rfc4514_name(std::string_view oid) noexcept
{
    static constexpr std::array<std::pair<std::string_view, std::string_view>, 9> names{
        {
         {"2.5.4.3", "CN"},
         {"2.5.4.7", "L"},
         {"2.5.4.8", "ST"},
         {"2.5.4.10", "O"},
         {"2.5.4.11", "OU"},
         {"2.5.4.6", "C"},
         {"2.5.4.9", "STREET"},
         {"0.9.2342.19200300.100.1.25", "DC"},
         {"0.9.2342.19200300.100.1.1", "UID"},
         }
    };

    const auto* found = std::ranges::find(names, oid, &std::pair<std::string_view, std::string_view>::first);
    return found != names.end() ? found->second : std::string_view{};
}

// append_escaped appends an attribute's value as RFC 4514 escapes it (section 2.4): its special characters, and a
// space or '#' first or a space last, with a backslash; and (as it allows) control characters as hex pairs, so a name
// can't break a line of a log.
void append_escaped(std::string& out, std::string_view value)
{
    for (std::size_t i = 0; i < value.size(); ++i)
    {
        const char c    = value[i];
        const auto byte = static_cast<unsigned char>(c);
        if (byte < 0x20 || byte == 0x7F)
        {
            const auto pair = net::encoding::hex::encode(static_cast<std::byte>(byte));
            out += '\\';
            out.append(pair.data(), pair.size());
            continue;
        }

        const bool special = std::string_view{"\"+,;<>\\"}.contains(c) || (i == 0 && (c == ' ' || c == '#'))
                          || (i + 1 == value.size() && c == ' ');
        if (special) out += '\\';
        out += c;
    }
}

// format_name writes a distinguished name (its DER: a sequence of relative distinguished names, each a set of
// attributes) as RFC 4514 does: the last RDN first, separated by commas, and each RDN's attributes separated by '+'.
// NOTE: from the DER, rather than Botan's X509_DN, whose attributes don't keep which RDN they're in.
std::string format_name(std::span<const std::uint8_t> der)
{
    std::vector<std::string> rdns;

    Botan::BER_Decoder decoder{der};
    Botan::BER_Decoder sequence = decoder.start_sequence();
    while (sequence.more_items())
    {
        Botan::BER_Decoder set = sequence.start_set();
        std::string        rdn;
        while (set.more_items())
        {
            Botan::OID                oid;
            std::vector<std::uint8_t> value;
            set.start_sequence().decode(oid).raw_bytes(value).end_cons();

            if (!rdn.empty()) rdn += '+';

            const auto type = oid.to_string();
            if (const auto name = rfc4514_name(type); !name.empty())
            {
                // NOTE: Botan only reads certificates whose names' values are strings.
                Botan::ASN1_String str;
                Botan::BER_Decoder{value}.decode(str).verify_end();

                rdn += name;
                rdn += '=';
                append_escaped(rdn, str.value());
            }
            else
            {
                rdn += type;
                rdn += "=#";
                rdn += net::encoding::hex::encode(std::as_bytes(std::span{value}));
            }
        }
        rdns.push_back(std::move(rdn));
    }

    std::string out;
    for (const auto& rdn : rdns | std::views::reverse)
    {
        if (!out.empty()) out += ',';
        out += rdn;
    }
    return out;
}

// to_time_point is when an X.509 time is, as the system clock's time: its earliest or latest if it can't hold it.
// NOTE: not Botan's to_std_timepoint(), which fails for times before 1970, or that a time_t can't hold.
std::chrono::system_clock::time_point to_time_point(const Botan::X509_Time& t)
{
    using clock = std::chrono::system_clock;
    using std::chrono::seconds;

    // NOTE: "YYYY/MM/DD hh:mm:ss UTC".
    const auto text  = t.readable_string();
    const auto field = [&text](std::size_t at, std::size_t length)
    {
        int value = 0;
        for (const char c : std::string_view{text}.substr(at, length)) value = (value * 10) + (c - '0');
        return value;
    };

    const std::chrono::sys_days    day{std::chrono::year{field(0, 4)} / field(5, 2) / field(8, 2)};
    const std::chrono::sys_seconds when =
        day + std::chrono::hours{field(11, 2)} + std::chrono::minutes{field(14, 2)} + seconds{field(17, 2)};

    constexpr auto earliest = std::chrono::ceil<seconds>(clock::time_point::min());
    constexpr auto latest   = std::chrono::floor<seconds>(clock::time_point::max());
    if (when < earliest) return clock::time_point::min();
    if (when > latest) return clock::time_point::max();
    return std::chrono::time_point_cast<clock::duration>(when);
}

// make_certificate is cert, as data.
net::tls::certificate make_certificate(const Botan::X509_Certificate& cert)
{
    net::tls::certificate out;

    const auto der = cert.BER_encode();
    out.der.resize(der.size());
    std::ranges::copy(std::as_bytes(std::span{der}), out.der.begin());

    out.subject = format_name(cert.raw_subject_dn());
    out.issuer  = format_name(cert.raw_issuer_dn());

    const auto& serial = cert.serial_number();
    out.serial_number  = serial.empty() ? "00" : net::encoding::hex::encode(std::as_bytes(std::span{serial}));

    out.not_before = to_time_point(cert.not_before());
    out.not_after  = to_time_point(cert.not_after());

    const auto& names = cert.subject_alt_name();
    out.dns_names.assign(names.dns().begin(), names.dns().end());
    out.email_addresses.assign(names.email().begin(), names.email().end());
    out.uris.assign(names.uris().begin(), names.uris().end());
    for (const auto v4 : names.ipv4_address()) out.ip_addresses.emplace_back(net::ipv4_addr{v4});
    for (const auto& v6 : names.ipv6_address())
    {
        std::array<std::uint8_t, 16> bytes{};
        std::ranges::copy(v6.address(), bytes.begin());
        out.ip_addresses.emplace_back(net::ipv6_addr{bytes});
    }

    const auto hash = Botan::HashFunction::create_or_throw("SHA-256");
    hash->update(der);
    const auto digest = hash->final_stdvec();
    std::ranges::copy(std::as_bytes(std::span{digest}).first(out.sha256.size()), out.sha256.begin());

    return out;
}

// no_certificates is an empty chain of certificates, shared without allocating (it's owned by nothing).
std::shared_ptr<const std::vector<net::tls::certificate>> no_certificates() noexcept
{
    static const std::vector<net::tls::certificate> none;
    return {std::shared_ptr<void>{}, &none};
}

// clock_function is what time it is (see detail::server_hooks): the system clock's, if it's empty.
using clock_function = std::function<std::chrono::system_clock::time_point()>;

std::chrono::system_clock::time_point now(const clock_function& clock)
{
    return clock ? clock() : std::chrono::system_clock::now();
}

// ticket_binding is what a server's tickets' keys are derived with besides the keys themselves (see ticket_manager): a
// label, its client authentication mode, and (if it asks for clients' certificates) a hash of its client CAs'
// certificates, in their file's order.
std::vector<std::uint8_t> ticket_binding(net::tls::client_auth auth, const std::vector<Botan::X509_Certificate>& cas)
{
    constexpr std::string_view label = "cppnet TLS session ticket key";

    std::vector<std::uint8_t> out(label.begin(), label.end());
    out.push_back(static_cast<std::uint8_t>(auth));

    if (!cas.empty())
    {
        // NOTE: each DER encoding says where it ends, so their concatenation is unambiguous.
        const auto hash = Botan::HashFunction::create_or_throw("SHA-256");
        for (const auto& ca : cas) hash->update(ca.BER_encode());
        const auto digest = hash->final_stdvec();
        out.insert(out.end(), digest.begin(), digest.end());
    }
    return out;
}

// client_sessions is a client's cache of sessions, to resume them: Botan's in-memory cache (of 1000, the oldest dropped
// first), but storing TLS 1.3's tickets as tickets. NOTE: Botan's TLS 1.3 client stores a ticket as an opaque handle,
//       which Botan 3.12's in-memory cache can't remove (its remove() finds a ticket only among handles stored as
//       tickets, or an opaque one only if it's as short as a session ID, 32 bytes, which tickets usually aren't). So a
//       ticket was never used up (as the policy's reuse_session_tickets(), false, means it to be), and the client
//       offered any of those it had been given (that hadn't expired).
class client_sessions final : public Botan::TLS::Session_Manager
{
public:
    explicit client_sessions(const std::shared_ptr<Botan::RandomNumberGenerator>& rng)
        : Botan::TLS::Session_Manager{rng}
        , cache{rng}
    {}

    void store(const Botan::TLS::Session& session, const Botan::TLS::Session_Handle& handle) override
    {
        if (handle.is_opaque_handle())
            cache.store(session, Botan::TLS::Session_Handle{Botan::TLS::Session_Ticket{handle.opaque_handle().get()}});
        else cache.store(session, handle);
    }

    std::vector<Botan::TLS::Session_with_Handle> find(const Botan::TLS::Server_Information& info,
                                                      Botan::TLS::Callbacks&                callbacks,
                                                      const Botan::TLS::Policy&             policy) override
    {
        return cache.find(info, callbacks, policy);
    }

    std::optional<Botan::TLS::Session> retrieve(const Botan::TLS::Session_Handle& handle,
                                                Botan::TLS::Callbacks&            callbacks,
                                                const Botan::TLS::Policy&         policy) override
    {
        return cache.retrieve(handle, callbacks, policy);
    }

    std::size_t remove(const Botan::TLS::Session_Handle& handle) override { return cache.remove(handle); }
    std::size_t remove_all() override { return cache.remove_all(); }

protected:
    // NOTE: unused, as find() and retrieve() are the cache's.
    std::optional<Botan::TLS::Session> retrieve_one(const Botan::TLS::Session_Handle& /*handle*/) override
    {
        return std::nullopt;
    }
    std::vector<Botan::TLS::Session_with_Handle> find_some(const Botan::TLS::Server_Information& /*info*/,
                                                           std::size_t /*max_sessions_hint*/) override
    {
        return {};
    }

private:
    Botan::TLS::Session_Manager_In_Memory cache;
};

// peer_port is the port a socket's connected to: 0 if it can't say.
std::uint16_t peer_port(const net::tcp_socket& socket) noexcept
{
    net::detail::socket_address addr;
    addr.size = sizeof(addr.storage);
    if (::getpeername(socket.native_handle(), addr.get(), &addr.size) != 0) return 0;

    const auto peer = net::detail::from_sockaddr(addr);
    return peer.has_value() ? peer->port : 0;
}

}

namespace net::tls::detail
{

// ticket_manager is a server's session manager: it makes TLS 1.3 session tickets (the session, encrypted with a key of
// its own: Botan's Session::encrypt()), and resumes sessions from them, storing nothing. TLS 1.2 sessions aren't
// resumed (see server_options::session_tickets).
//
// Its keys are the given ones, or a random one, replaced once it's a lifetime old (lazily, as tickets are made or
// resumed, as Go's are, so there's no timer), the previous one kept until it's two lifetimes old: as no key makes
// tickets once it's a lifetime old, every ticket can be resumed for its lifetime, and no longer. Each is derived from
// the key it's given (or the random one) and the context's ticket_binding(), with HMAC-SHA-256, so a ticket made by a
// context that verifies clients' certificates differently (another mode, or other CAs) doesn't decrypt.
// NOTE: a member of server_state, which the header declares, so it mustn't be internal.
// NOLINTNEXTLINE(misc-use-internal-linkage)
class ticket_manager final : public Botan::TLS::Session_Manager
{
public:
    ticket_manager(const std::shared_ptr<Botan::RandomNumberGenerator>& rng,
                   clock_function                                       clock,
                   std::chrono::seconds                                 lifetime,
                   net::tls::client_auth                                auth,
                   std::vector<std::uint8_t>                            binding,
                   std::vector<net::tls::session_ticket_key>            given)
        : Botan::TLS::Session_Manager{rng}
        , clock{std::move(clock)}
        , lifetime{lifetime}
        , auth{auth}
        , binding{std::move(binding)}
    {
        if (given.empty())
        {
            auto made = std::make_shared<key_set>();
            made->keys.push_back(random_key(now(this->clock)));
            made->rotating = true;
            keys           = std::move(made);
        }
        else
        {
            set_keys(std::move(given));
        }
    }

    // set_keys replaces the keys with those given (zeroing them once the keys are derived from them), stopping the
    // rotation.
    void set_keys(std::vector<net::tls::session_ticket_key> given)
    {
        util::defer scrub{[&given] noexcept
                          { Botan::secure_scrub_memory(given.data(), given.size() * sizeof(given.front())); }};

        auto made = std::make_shared<key_set>();
        for (const auto& key : given) made->keys.push_back(derive(key));

        std::scoped_lock lock{mu};
        keys = std::move(made);
    }

    // NOTE: only TLS 1.3 sessions get tickets, and none get session IDs (the handle a stateful server keeps).
    std::optional<Botan::TLS::Session_Handle> establish(const Botan::TLS::Session& session,
                                                        const std::optional<Botan::TLS::Session_ID>& /*id*/,
                                                        bool /*tls12_no_ticket*/) override
    {
        if (!session.version().is_tls_13_or_later()) return std::nullopt;

        try
        {
            const auto current = current_keys();
            if (current->keys.empty()) return std::nullopt;

            return Botan::TLS::Session_Handle{
                Botan::TLS::Session_Ticket{session.encrypt(current->keys.front().key, *m_rng)}};
        }
        catch (const std::exception&)
        {
            // no ticket, rather than failing the handshake.
            return std::nullopt;
        }
    }

    // NOTE: only clients store sessions.
    void store(const Botan::TLS::Session& /*session*/, const Botan::TLS::Session_Handle& /*handle*/) override {}

    std::size_t remove(const Botan::TLS::Session_Handle& /*handle*/) override { return 0; }
    std::size_t remove_all() override { return 0; }

    // NOTE: only TLS 1.2's server asks (TLS 1.3's sends tickets if establish() gives one): false, so it doesn't say
    //       it'll send one (and then send an empty one).
    bool emits_session_tickets() override { return false; }

protected:
    // NOTE: Botan's retrieve() then checks the ticket's age (against the policy's session_ticket_lifetime(), by the
    //       callbacks' clock), and its server name and cipher suite's hash.
    std::optional<Botan::TLS::Session> retrieve_one(const Botan::TLS::Session_Handle& handle) override
    {
        const auto ticket = handle.ticket();
        if (!ticket.has_value()) return std::nullopt;

        const auto current = current_keys();

        std::optional<Botan::TLS::Session> session;
        for (const auto& key : current->keys)
        {
            session = decrypt(ticket->get(), key.key);
            if (session.has_value()) break;
        }

        // RFC 8446 4.2.11: a ticket that can't be decrypted is ignored (a full handshake).
        if (!session.has_value() || !session->version().is_tls_13_or_later()) return std::nullopt;

        // as Go's server, nothing that wouldn't be accepted now: no client certificate where one's required, or one
        // that has expired since.
        const auto& certs = session->peer_certs();
        if (certs.empty())
        {
            if (auth == net::tls::client_auth::required) return std::nullopt;
        }
        else if (now(clock) > to_time_point(certs.front().not_after()))
        {
            return std::nullopt;
        }

        return session;
    }

    // NOTE: only clients look for sessions.
    std::vector<Botan::TLS::Session_with_Handle> find_some(const Botan::TLS::Server_Information& /*info*/,
                                                           std::size_t /*max_sessions_hint*/) override
    {
        return {};
    }

private:
    struct key
    {
        Botan::SymmetricKey                   key;
        std::chrono::system_clock::time_point made;
    };

    // key_set is the keys, the first making tickets; rotating if they're random ones, replaced in time.
    struct key_set
    {
        std::vector<key> keys;
        bool             rotating = false;
    };

    // decrypt is the session in ticket, if key made it. NOTE: one that didn't fails at once, on the key name the ticket
    //       carries (derived from the key).
    static std::optional<Botan::TLS::Session> decrypt(std::span<const std::uint8_t> ticket,
                                                      const Botan::SymmetricKey&    key)
    {
        try
        {
            return Botan::TLS::Session::decrypt(ticket, key);
        }
        catch (const std::exception&)
        {
            return std::nullopt;
        }
    }

    // derive is the key tickets are encrypted with, from a key that's given (or random), made at made.
    [[nodiscard]] key derive(std::span<const std::byte> given, std::chrono::system_clock::time_point made = {}) const
    {
        const auto hmac = Botan::MessageAuthenticationCode::create_or_throw("HMAC(SHA-256)");
        hmac->set_key(reinterpret_cast<const std::uint8_t*>(given.data()), given.size());
        hmac->update(binding);
        return key{.key = Botan::SymmetricKey{hmac->final()}, .made = made};
    }

    [[nodiscard]] key random_key(std::chrono::system_clock::time_point made) const
    {
        const auto random = m_rng->random_vec(sizeof(net::tls::session_ticket_key));
        return derive(std::as_bytes(std::span{random}), made);
    }

    // current_keys is the keys to use now: if they're random, the first replaced once it's a lifetime old, and the
    // previous dropped once it's two.
    std::shared_ptr<const key_set> current_keys()
    {
        std::scoped_lock lock{mu};
        if (!keys->rotating) return keys;

        const auto at       = now(clock);
        const auto newest   = keys->keys.front().made;
        const bool rotate   = at - newest >= lifetime;
        const bool previous = keys->keys.size() > 1;
        const bool drop     = previous && at - keys->keys[1].made >= 2 * lifetime;
        if (!rotate && !drop) return keys;

        auto next      = std::make_shared<key_set>();
        next->rotating = true;
        if (rotate)
        {
            next->keys.push_back(random_key(at));
            if (at - newest < 2 * lifetime) next->keys.push_back(keys->keys.front());
        }
        else
        {
            next->keys.push_back(keys->keys.front());
        }

        keys = std::move(next);
        return keys;
    }

    clock_function            clock;
    std::chrono::seconds      lifetime;
    net::tls::client_auth     auth;
    std::vector<std::uint8_t> binding;

    // NOTE: replaced as a whole (under mu), so a handshake uses the keys it took without holding mu.
    std::mutex                     mu;
    std::shared_ptr<const key_set> keys;
};

struct client_state
{
    std::shared_ptr<Botan::RandomNumberGenerator> rng;
    std::shared_ptr<Botan::Credentials_Manager>   credentials;
    std::shared_ptr<Botan::TLS::Session_Manager>  sessions;
    std::shared_ptr<const Botan::TLS::Policy>     policy;
    bool                                          skip_verification = false;
    std::vector<std::string>                      alpn;
};

struct server_state
{
    std::shared_ptr<Botan::RandomNumberGenerator> rng;
    std::shared_ptr<Botan::Credentials_Manager>   credentials;
    std::shared_ptr<Botan::TLS::Session_Manager>  sessions;
    std::shared_ptr<ticket_manager>               tickets; // sessions, if they're resumed: null otherwise
    std::shared_ptr<const Botan::TLS::Policy>     policy;
    std::vector<std::string>                      alpn;
    std::shared_ptr<const server_hooks>           hooks; // null without any
    Botan::X509_Certificate                       leaf;  // the server's certificate (its chain's first), for covers()
};

namespace
{

    // verify_chain verifies a peer's certificate chain as Botan's Callbacks::tls_verify_cert_chain does (with
    // x509_path_validate() and the policy's restrictions), at now, and for a server's certificate, for hostname: for
    // the handshake, and for verify_for() (another host, as a handshake would verify it, so they share this). The chain
    // mustn't be empty.
    Botan::Path_Validation_Result verify_chain(const std::vector<Botan::X509_Certificate>&              chain,
                                               const std::vector<std::optional<Botan::OCSP::Response>>& ocsp_responses,
                                               const std::vector<Botan::Certificate_Store*>&            trusted_roots,
                                               Botan::Usage_Type                                        usage,
                                               std::string_view                                         hostname,
                                               const Botan::TLS::Policy&                                policy,
                                               std::chrono::system_clock::time_point                    now,
                                               std::chrono::milliseconds                                ocsp_timeout)
    {
        const Botan::Path_Validation_Restrictions restrictions{policy.require_cert_revocation_info(),
                                                               policy.minimum_signature_strength()};

        // the host name is only matched against a server's certificate.
        const auto name = usage == Botan::Usage_Type::TLS_CLIENT_AUTH ? std::string_view{} : hostname;
        return Botan::x509_path_validate(chain,
                                         restrictions,
                                         trusted_roots,
                                         name,
                                         usage,
                                         now,
                                         ocsp_timeout,
                                         ocsp_responses);
    }

}

// callbacks are Botan's calls back into a connection. They're only made while the connection's mutex is held (from
// inside the channel's methods, which are only called holding it), and only touch what it guards.
// NOTE: a member of connection, which the header declares, so it mustn't be internal.
// NOLINTNEXTLINE(misc-use-internal-linkage)
class callbacks final : public Botan::TLS::Callbacks
{
public:
    callbacks(connection*                         conn,
              bool                                skip_verification,
              std::vector<std::string>            server_alpn,
              std::shared_ptr<const server_hooks> hooks = nullptr) noexcept
        : conn{conn}
        , skip_verification{skip_verification}
        , server_alpn{std::move(server_alpn)}
        , hooks{std::move(hooks)}
    {}

    // NOTE: for a ticket's age, and verifying certificates.
    std::chrono::system_clock::time_point tls_current_timestamp() override
    {
        return hooks ? now(hooks->clock) : Botan::TLS::Callbacks::tls_current_timestamp();
    }

    void tls_emit_data(std::span<const std::uint8_t> data) override;
    void tls_record_received(std::uint64_t seq_no, std::span<const std::uint8_t> data) override;
    void tls_alert(Botan::TLS::Alert alert) override;
    bool tls_peer_closed_connection() override;
    void tls_session_established(const Botan::TLS::Session_Summary& session) override;

    // NOTE: only for server_hooks::handshake_done (a server's): it sees the messages sent as well as those received.
    void tls_inspect_handshake_msg(const Botan::TLS::Handshake_Message& message) override
    {
        if (message.type() == Botan::TLS::Handshake_Type::HelloRetryRequest) hello_retry = true;
    }

    // record_verification_failure keeps why the peer's certificate was rejected.
    void record_verification_failure(Botan::Certificate_Status_Code status) noexcept;

    void tls_verify_cert_chain(const std::vector<Botan::X509_Certificate>&              cert_chain,
                               const std::vector<std::optional<Botan::OCSP::Response>>& ocsp_responses,
                               const std::vector<Botan::Certificate_Store*>&            trusted_roots,
                               Botan::Usage_Type                                        usage,
                               std::string_view                                         hostname,
                               const Botan::TLS::Policy&                                policy) override
    {
        if (skip_verification) return;

        // as Botan's Callbacks::tls_verify_cert_chain does, keeping why it failed (which its exception only has as
        // text).
        if (cert_chain.empty()) throw Botan::Invalid_Argument("Certificate chain was empty");

        const auto result = verify_chain(cert_chain,
                                         ocsp_responses,
                                         trusted_roots,
                                         usage,
                                         hostname,
                                         policy,
                                         tls_current_timestamp(),
                                         tls_verify_cert_chain_ocsp_timeout());
        if (!result.successful_validation())
        {
            record_verification_failure(result.result());
            throw Botan::TLS::TLS_Exception(Botan::TLS::Alert::BadCertificate,
                                            "Certificate validation failure: " + result.result_string());
        }
    }

    std::string tls_server_choose_app_protocol(const std::vector<std::string>& client_protos) override
    {
        for (const auto& protocol : server_alpn)
        {
            if (std::ranges::find(client_protos, protocol) != client_protos.end()) return protocol;
        }

        // none in common: no ALPN, rather than failing (e.g. a client that doesn't offer http/1.1, but speaks it).
        return "";
    }

private:
    connection*                         conn;
    bool                                skip_verification;
    std::vector<std::string>            server_alpn;
    std::shared_ptr<const server_hooks> hooks;
    bool                                hello_retry = false; // whether a HelloRetryRequest was sent (or received)
};

struct connection
{
    explicit connection(tcp_socket conn) noexcept
        : socket{std::move(conn)}
        , flushed{socket.get_scheduler()->pool()}
    {}

    // take_received moves what's been decrypted (and not read yet) into data, returning how much. Must be called
    // holding mu.
    std::size_t take_received(std::span<std::byte> data) noexcept
    {
        const auto count = std::min(data.size(), received.size() - received_start);
        std::ranges::copy(std::as_bytes(std::span{received}).subspan(received_start, count), data.begin());

        received_start += count;
        if (received_start == received.size())
        {
            received.clear();
            received_start = 0;
        }

        return count;
    }

    // feed passes ciphertext read from the socket to the channel. Must be called holding mu.
    void feed(std::span<const std::uint8_t> ciphertext) noexcept
    {
        track_records(ciphertext);

        try
        {
            channel->received_data(ciphertext);
        }
        catch (const Botan::TLS::TLS_Exception& ex)
        {
            if (!failure)
            {
                failure = verification_failure
                            ? std::error_code{static_cast<int>(*verification_failure), verification_category()}
                            : sent_alert(ex.type());
            }
        }
        catch (const Botan::Invalid_Authentication_Tag&)
        {
            // a record that couldn't be decrypted, for which Botan sent bad_record_mac.
            if (!failure) failure = sent_alert(Botan::TLS::Alert::BadRecordMac);
        }
        catch (const std::exception&)
        {
            if (!failure) failure = make_error_code(errc::protocol_error);
        }
    }

    // track_records follows TLS's record framing (the same in every version: a 5-byte header, ending with the
    // fragment's length) through the ciphertext fed to the channel, so idle_clean() knows whether the channel holds
    // part of a record. NOTE: received_data()'s hint can't say: TLS 1.3's is 5 both between records and with 5 bytes of
    // one to come. Must be called holding mu.
    void track_records(std::span<const std::uint8_t> ciphertext) noexcept
    {
        while (!ciphertext.empty())
        {
            if (record_left > 0)
            {
                const auto n = std::min(record_left, ciphertext.size());
                record_left -= n;
                ciphertext = ciphertext.subspan(n);
                continue;
            }

            const auto n = std::min(record_header.size() - header_have, ciphertext.size());
            std::ranges::copy(ciphertext.first(n), record_header.begin() + static_cast<std::ptrdiff_t>(header_have));
            header_have += n;
            ciphertext = ciphertext.subspan(n);

            if (header_have == record_header.size())
            {
                record_left = (std::size_t{record_header[3]} << 8U) | record_header[4];
                header_have = 0;
            }
        }
    }

    // flush_step is what a flush does next: return (with err), write pending (having taken the flush on), or wait
    // for the flush in progress (whose notifier's value was seen).
    struct flush_step
    {
        enum class action : std::uint8_t
        {
            done,
            write,
            wait,
        };

        action                    what = action::done;
        std::error_code           err;
        std::vector<std::uint8_t> pending;
        std::uint64_t             seen = 0;
    };

    // next_flush_step decides what a flush that's done once emitted reaches target does next. Must be called holding
    // mu.
    flush_step next_flush_step(std::uint64_t target, bool wait)
    {
        flush_step step;
        if (write_failure)
        {
            step.err = write_failure;
            return step;
        }

        // nothing that's queued is left behind: a flush in progress writes until nothing is, or hands on to those
        // waiting, the first of which writes the rest (whether its own has gone or not).
        if (!flushing && !outgoing.empty())
        {
            flushing  = true;
            step.what = flush_step::action::write;
            step.pending.swap(outgoing);
            return step;
        }

        if (written >= target || !wait) return step;

        ++flush_waiters;
        step.what = flush_step::action::wait;
        step.seen = flushed.value();
        return step;
    }

    // flush writes what the channel has emitted (handshake messages, records, alerts) to the socket, returning once
    // emitted reaches target. One call writes at a time (so they go out in the order they were emitted); the others
    // wait for it (if wait), as far as their own: so each is limited by the socket's write deadline and timeout, and
    // the writes it waits for fail it. Without wait (a reader's replies), it leaves what it emitted to a flush in
    // progress: a reader that waited for a writer stuck on a peer that's waiting for its writes to be read would never
    // read them.
    coro::task<std::error_code> flush(std::uint64_t target, bool wait)
    {
        flush_step step;
        {
            std::scoped_lock lock{mu};
            step = next_flush_step(target, wait);
        }

        while (true)
        {
            switch (step.what)
            {
            case flush_step::action::done: co_return step.err;
            case flush_step::action::write: co_return co_await write_queued(std::move(step.pending), target);
            case flush_step::action::wait: (void)co_await flushed.wait(step.seen); break;
            }

            std::scoped_lock lock{mu};
            --flush_waiters;
            step = next_flush_step(target, wait);
        }
    }

    // flush_emitted flushes everything emitted so far.
    coro::task<std::error_code> flush_emitted(bool wait)
    {
        std::uint64_t target = 0;
        {
            std::scoped_lock lock{mu};
            target = emitted;
        }

        return flush(target, wait);
    }

    // write_queued writes pending, having taken the flush on, then what's queued meanwhile: until nothing is, or (once
    // emitted has reached target) others wait to take it on.
    coro::task<std::error_code> write_queued(std::vector<std::uint8_t> pending, std::uint64_t target)
    {
        // if this is destroyed while it writes (its task was), the rest of a record may never go, so nothing more can.
        bool        ended = false;
        util::defer abandoned{[this, &ended] noexcept
                              {
                                  if (ended) return;
                                  {
                                      std::scoped_lock lock{mu};
                                      if (!write_failure)
                                          write_failure = std::make_error_code(std::errc::operation_canceled);
                                      flushing = false;
                                  }
                                  flushed.notify();
                              }};

        while (true)
        {
            auto res = co_await io::write_all(socket, std::as_bytes(std::span{pending}));

            {
                std::scoped_lock lock{mu};
                if (res.err)
                {
                    // part of a record may have gone.
                    write_failure = res.err;
                    flushing      = false;
                    ended         = true;
                }
                else
                {
                    written += pending.size();
                    pending.clear();

                    if (outgoing.empty() || (written >= target && flush_waiters > 0))
                    {
                        flushing = false;
                        ended    = true;
                    }
                    else
                    {
                        pending.swap(outgoing);
                    }
                }
            }

            // those whose writes have gone return; and those waiting take the rest on, if this has stopped.
            flushed.notify();
            if (ended) co_return res.err;
        }
    }

    // wait_writable waits until the socket's writable (see socket::wait_writable()), as a flush: once no flush is in
    // progress, taking the flush on while it waits, so it never waits alongside one of the socket's writes (whichever
    // came second would fail with std::errc::device_or_resource_busy), nor does a read's reply (which leaves what it
    // emitted to it, as to any flush in progress). What's queued by the time it's writable is written before it
    // returns (the flush's, so its failure is the stream's).
    coro::task<io::result> wait_writable()
    {
        while (true)
        {
            std::uint64_t seen = 0;
            {
                std::scoped_lock lock{mu};
                if (write_failure) co_return io::result{.err = write_failure};
                if (!flushing)
                {
                    flushing = true;
                    break;
                }

                ++flush_waiters;
                seen = flushed.value();
            }

            (void)co_await flushed.wait(seen);

            std::scoped_lock lock{mu};
            --flush_waiters;
        }

        // the flush is this one's until it's handed to write_queued(), or released (also if this is destroyed while it
        // waits: having written nothing, it fails nothing).
        bool        held = true;
        util::defer release{[this, &held] noexcept
                            {
                                if (!held) return;
                                {
                                    std::scoped_lock lock{mu};
                                    flushing = false;
                                }
                                flushed.notify();
                            }};

        const auto                res = co_await socket.wait_writable();
        std::vector<std::uint8_t> pending;
        std::uint64_t             target = 0;
        if (!res.err)
        {
            std::scoped_lock lock{mu};
            pending.swap(outgoing);
            target = emitted;
        }
        if (pending.empty()) co_return res;

        held = false;
        if (auto err = co_await write_queued(std::move(pending), target); err) co_return io::result{.err = err};
        co_return res;
    }

    // receive reads from the socket once, and passes what it read to the channel, returning the read's error (if any).
    coro::task<std::error_code> receive()
    {
        // NOTE: only one read waits at once (as for the socket), so ciphertext isn't shared.
        auto res = co_await socket.read(std::as_writable_bytes(std::span{ciphertext}));

        if (res.count > 0)
        {
            bool replied = false;
            {
                std::scoped_lock lock{mu};
                const auto       before = emitted;
                feed(std::span{ciphertext}.first(res.count));
                replied = emitted != before;
            }

            // e.g. handshake messages, or an alert, in reply.
            if (replied)
            {
                if (auto err = co_await flush_emitted(false); err && !res.err) res.err = err;
            }
        }

        co_return res.err;
    }

    // handshake runs the handshake until it's done, deadline passes, stop is requested, or it fails.
    coro::task<std::error_code> handshake(io::clock::time_point deadline, std::stop_token stop)
    {
        socket.set_deadline(deadline);
        util::defer no_deadline{[this] noexcept { socket.set_deadline(std::nullopt); }};

        // NOTE: ending the socket's incoming side ends a read waiting for the peer (it finds the end of the stream).
        std::stop_callback on_stop{stop, [this] noexcept { socket.shutdown_read(); }};

        auto err = co_await handshake_steps();
        if (err && stop.stop_requested()) co_return std::make_error_code(std::errc::operation_canceled);
        co_return err;
    }

    // handshake_steps sends and receives the handshake's messages until it's done, or fails.
    coro::task<std::error_code> handshake_steps()
    {
        // e.g. the client's hello.
        if (auto err = co_await flush_emitted(true); err) co_return err;

        while (true)
        {
            {
                std::scoped_lock lock{mu};
                if (failure) co_return failure;

                // NOTE: not is_active(), which is false once the peer has closed: in TLS 1.3, a server may send data
                //       (and close_notify) straight after its handshake messages, before the client has finished its
                //       own, so they may arrive together. What came after the handshake is left for read().
                if (channel->is_handshake_complete())
                {
                    alpn = channel->application_protocol();

                    // NOTE: copies of Botan's certificates share their data, so this is a vector of pointers.
                    try
                    {
                        peer_chain = channel->peer_cert_chain();
                    }
                    catch (const std::exception&)
                    {
                        failure = make_error_code(errc::protocol_error);
                        co_return failure;
                    }
                    co_return std::error_code{};
                }
                if (peer_closed) co_return make_error_code(io::errc::end_of_stream);
            }

            if (auto err = co_await receive(); err)
            {
                std::scoped_lock lock{mu};
                if (failure) co_return failure;
                co_return err;
            }
        }
    }

    // read_more reads from the socket until there's decrypted data for data, the stream ends, or an error.
    coro::task<io::result> read_more(std::span<std::byte> data)
    {
        while (true)
        {
            auto err = co_await receive();

            {
                std::scoped_lock lock{mu};
                if (auto count = take_received(data); count > 0) co_return io::result{.count = count};
                if (failure) co_return io::result{.err = failure};
                if (peer_closed) co_return io::result{.err = make_error_code(io::errc::end_of_stream)};
            }

            // the connection ended without close_notify: what was read may have been cut short.
            if (err == io::errc::end_of_stream) co_return io::result{.err = make_error_code(errc::stream_truncated)};
            if (err) co_return io::result{.err = err};
        }
    }

    // NOTE: declared first, so destroyed last: the channel (below) may use the rest until it's gone.
    tcp_socket socket;

    std::mutex mu;

    // Guarded by mu (and only touched by the callbacks, or holding it).
    std::vector<std::uint8_t>                     received; // decrypted data not read yet, from received_start
    std::size_t                                   received_start = 0;
    bool                                          peer_closed    = false; // the peer's close_notify
    bool                                          hold_reply     = false; // see accept_options::hold_close_reply
    std::error_code                               failure;                // a fatal alert, or a broken protocol
    std::optional<Botan::Certificate_Status_Code> verification_failure;   // why the peer's certificate was rejected
    std::string                                   alpn;

    // the chain the peer sent (set once the handshake is complete), and as peer_certificates() gives it, once it has.
    std::vector<Botan::X509_Certificate>            peer_chain;
    std::shared_ptr<const std::vector<certificate>> peer_certs;

    // the version the handshake agreed on, and whether it resumed a session. NOTE: set by the handshake, before the
    //       stream is made, and never after (the policy refuses renegotiation), so they're read without mu.
    version negotiated = version::tls13;
    bool    resumed    = false;

    // the record the channel has been fed part of: the part of its header (header_have bytes of it) or how much of its
    // fragment is still to come.
    std::array<std::uint8_t, 5> record_header{};
    std::size_t                 header_have = 0;
    std::size_t                 record_left = 0;

    // ciphertext to write, emitted counting all there has been, and written how much of that has been written. One
    // flush writes at a time (flushing), which flush_waiters wait for.
    std::vector<std::uint8_t> outgoing;
    std::uint64_t             emitted       = 0;
    std::uint64_t             written       = 0;
    bool                      flushing      = false;
    std::size_t               flush_waiters = 0;
    std::error_code           write_failure; // a write that failed, which may have sent part of a record

    // notified when a flush has written some, or stopped.
    coro::notifier flushed;

    // what a read of the socket reads into: only one waits at once.
    std::array<std::uint8_t, (std::size_t{16} * 1'024) + 1'024> ciphertext{};

    std::shared_ptr<callbacks>           cb;
    std::unique_ptr<Botan::TLS::Channel> channel;

    // a client's context (null for a server's stream), for verify_for(): its trust store and policy. NOTE: set before
    //       the stream is made, and never after, so it's read without mu.
    std::shared_ptr<client_state> client;
};

void callbacks::tls_emit_data(std::span<const std::uint8_t> data)
{
    conn->outgoing.insert(conn->outgoing.end(), data.begin(), data.end());
    conn->emitted += data.size();
}

void callbacks::tls_record_received(std::uint64_t /*seq_no*/, std::span<const std::uint8_t> data)
{
    conn->received.insert(conn->received.end(), data.begin(), data.end());
}

void callbacks::record_verification_failure(Botan::Certificate_Status_Code status) noexcept
{
    conn->verification_failure = status;
}

void callbacks::tls_alert(Botan::TLS::Alert alert)
{
    if (alert.type() == Botan::TLS::Alert::CloseNotify) conn->peer_closed = true;
    else if (alert.is_fatal() && !conn->failure) conn->failure = peer_alert(alert.type());
}

void callbacks::tls_session_established(const Botan::TLS::Session_Summary& session)
{
    conn->negotiated = session.version() == Botan::TLS::Protocol_Version::TLS_V13 ? version::tls13 : version::tls12;
    conn->resumed    = session.was_resumption();

    if (!hooks || !hooks->handshake_done) return;

    // TLS 1.3's group, by name (none over TLS 1.2).
    const auto params = session.kex_parameters();
    const auto group  = params.has_value() ? Botan::TLS::Group_Params::from_string(*params) : std::nullopt;
    hooks->handshake_done({
        .resumed     = conn->resumed,
        .group       = group.has_value() ? group->wire_code() : std::uint16_t{0},
        .hello_retry = hello_retry,
    });
}

// NOTE: TLS 1.3's channel answers the peer's close_notify only if this returns true; TLS 1.2's always does (as RFC 5246
//       requires), whatever it returns.
bool callbacks::tls_peer_closed_connection() { return !conn->hold_reply; }

}

namespace net::tls
{

const std::error_category& category() noexcept
{
    static const tls_category_impl instance;
    return instance;
}

const std::error_category& verification_category() noexcept { return the_verification_category; }

std::error_code make_error_code(errc err) noexcept { return {static_cast<int>(err), category()}; }

const std::error_category& alert_category() noexcept { return the_alert_category; }

std::error_condition make_error_condition(alert a) noexcept { return {static_cast<int>(a), the_alert_category}; }

bool from_peer(std::error_code err) noexcept
{
    return err.category() == category() && err.value() >= 0 && err.value() <= 255;
}

client_context::client_context(std::shared_ptr<detail::client_state> state) noexcept
    : state{std::move(state)}
{}

std::expected<client_context, std::error_code> client_context::make(const client_options& options)
{
    return detail::make_client_context(options, {});
}

std::expected<client_context, std::error_code> detail::make_client_context(const client_options& options,
                                                                           const policy_limits&  limits)
{
    if (!valid_versions(options.min_version, options.max_version))
        return std::unexpected(std::make_error_code(std::errc::invalid_argument));

    // a certificate needs its key, and a key its certificate.
    if (options.certificate_chain.has_value() != options.private_key.has_value())
        return std::unexpected(std::make_error_code(std::errc::invalid_argument));

    identity own;
    if (options.certificate_chain.has_value())
    {
        auto loaded = load_identity(*options.certificate_chain, *options.private_key);
        if (!loaded.has_value()) return std::unexpected(loaded.error());
        own = std::move(*loaded);
    }

    try
    {
        std::unique_ptr<Botan::Certificate_Store> store;
        if (options.trusted_ca_file)
        {
            auto loaded = load_store(*options.trusted_ca_file);
            if (!loaded.has_value()) return std::unexpected(loaded.error());
            store = std::move(*loaded);
        }
        else
        {
            store = std::make_unique<Botan::System_Certificate_Store>();
        }

        auto rng   = std::make_shared<Botan::System_RNG>();
        auto state = std::make_shared<detail::client_state>(detail::client_state{
            .rng = rng,
            .credentials =
                std::make_shared<credentials>("tls-client", std::move(store), std::move(own.chain), std::move(own.key)),
            .sessions          = std::make_shared<client_sessions>(rng),
            .policy            = std::make_shared<policy>(options.require_extended_master_secret,
                                                          options.min_version,
                                                          options.max_version,
                                                          client_auth::none,
                                                          limits),
            .skip_verification = options.insecure_skip_verification,
            .alpn              = options.alpn,
        });

        return client_context{std::move(state)};
    }
    catch (const std::exception&)
    {
        // e.g. the system's trust store couldn't be read.
        return std::unexpected(std::make_error_code(std::errc::io_error));
    }
}

server_context::server_context(std::shared_ptr<detail::server_state> state) noexcept
    : state{std::move(state)}
{}

std::expected<server_context, std::error_code> server_context::make(const server_options& options)
{
    return detail::make_server_context(options, {});
}

std::expected<server_context, std::error_code> detail::make_server_context(const server_options& options,
                                                                           server_hooks          hooks)
{
    if (!valid_versions(options.min_version, options.max_version))
        return std::unexpected(std::make_error_code(std::errc::invalid_argument));

    // TLS 1.3's tickets last 7 days at most (RFC 8446 4.6.1); and keys are only used with tickets.
    constexpr std::chrono::seconds longest = std::chrono::days{7};
    if (options.session_ticket_lifetime <= std::chrono::seconds{0} || options.session_ticket_lifetime > longest
        || (!options.session_tickets && !options.session_ticket_keys.empty()))
        return std::unexpected(std::make_error_code(std::errc::invalid_argument));

    // client certificates are verified against client_ca_file's CAs, never the system's: so it's needed to ask for
    // them, and refused without asking (as it wouldn't be used).
    const bool asks = options.client_auth != client_auth::none;
    if (options.client_auth > client_auth::required || asks != options.client_ca_file.has_value())
        return std::unexpected(std::make_error_code(std::errc::invalid_argument));

    auto own = load_identity(options.certificate_chain, options.private_key);
    if (!own.has_value()) return std::unexpected(own.error());

    std::vector<Botan::X509_Certificate> client_cas;
    if (asks)
    {
        auto loaded = load_certificates(*options.client_ca_file);
        if (!loaded.has_value()) return std::unexpected(loaded.error());
        client_cas = std::move(*loaded);
    }

    try
    {
        auto rng = std::make_shared<Botan::System_RNG>();

        std::shared_ptr<const server_hooks> shared_hooks;
        if (hooks.clock || hooks.handshake_done) shared_hooks = std::make_shared<const server_hooks>(std::move(hooks));

        std::shared_ptr<ticket_manager> tickets;
        if (options.session_tickets)
        {
            tickets = std::make_shared<ticket_manager>(rng,
                                                       shared_hooks ? shared_hooks->clock : clock_function{},
                                                       options.session_ticket_lifetime,
                                                       options.client_auth,
                                                       ticket_binding(options.client_auth, client_cas),
                                                       options.session_ticket_keys);
        }

        std::shared_ptr<Botan::TLS::Session_Manager> sessions = tickets;
        if (!sessions) sessions = std::make_shared<Botan::TLS::Session_Manager_Noop>();

        auto leaf  = own->chain.front();
        auto state = std::make_shared<detail::server_state>(detail::server_state{
            .rng         = rng,
            .credentials = std::make_shared<credentials>("tls-server",
                                                         asks ? store_of(client_cas) : nullptr,
                                                         std::move(own->chain),
                                                         std::move(own->key)),
            .sessions    = std::move(sessions),
            .tickets     = std::move(tickets),
            .policy      = std::make_shared<policy>(options.require_extended_master_secret,
                                                    options.min_version,
                                                    options.max_version,
                                                    options.client_auth,
                                                    policy_limits{},
                                                    options.session_ticket_lifetime),
            .alpn        = options.alpn,
            .hooks       = std::move(shared_hooks),
            .leaf        = std::move(leaf),
        });

        return server_context{std::move(state)};
    }
    catch (const std::exception&)
    {
        return std::unexpected(std::make_error_code(std::errc::invalid_argument));
    }
}

void server_context::set_session_ticket_keys(std::vector<session_ticket_key> keys)
{
    if (state->tickets) state->tickets->set_keys(std::move(keys));
    else Botan::secure_scrub_memory(keys.data(), keys.size() * sizeof(session_ticket_key));
}

stream::stream(std::unique_ptr<detail::connection> conn) noexcept
    : conn{std::move(conn)}
{}

stream::stream(stream&&) noexcept            = default;
stream& stream::operator=(stream&&) noexcept = default;
stream::~stream()                            = default;

coro::task<std::expected<stream, std::error_code>> stream::connect(
    tcp_socket conn, client_context ctx, std::string host, io::clock::time_point deadline, std::stop_token stop)
{
    // the certificate is checked against host: without one, nothing would be.
    if (host.empty() && !ctx.state->skip_verification)
        co_return std::unexpected(std::make_error_code(std::errc::invalid_argument));

    auto c    = std::make_unique<detail::connection>(std::move(conn));
    c->client = ctx.state;

    try
    {
        c->cb = std::make_shared<detail::callbacks>(c.get(), ctx.state->skip_verification, std::vector<std::string>{});

        std::scoped_lock lock{c->mu};
        c->channel = std::make_unique<Botan::TLS::Client>(c->cb,
                                                          ctx.state->sessions,
                                                          ctx.state->credentials,
                                                          ctx.state->policy,
                                                          ctx.state->rng,
                                                          // NOTE: its sessions are kept by host and port, so a
                                                          //       ticket is only offered where it came from.
                                                          Botan::TLS::Server_Information{host, peer_port(c->socket)},
                                                          ctx.state->policy->latest_supported_version(false),
                                                          ctx.state->alpn);
    }
    catch (const std::exception&)
    {
        co_return std::unexpected(make_error_code(errc::protocol_error));
    }

    if (auto err = co_await c->handshake(deadline, std::move(stop)); err) co_return std::unexpected(err);
    co_return stream{std::move(c)};
}

coro::task<std::expected<stream, std::error_code>>
stream::accept(tcp_socket conn, server_context ctx, io::clock::time_point deadline, accept_options options)
{
    auto c        = std::make_unique<detail::connection>(std::move(conn));
    c->hold_reply = options.hold_close_reply;

    try
    {
        auto protocols = options.alpn.has_value() ? std::move(*options.alpn) : ctx.state->alpn;
        c->cb          = std::make_shared<detail::callbacks>(c.get(), false, std::move(protocols), ctx.state->hooks);

        std::scoped_lock lock{c->mu};
        c->channel = std::make_unique<Botan::TLS::Server>(c->cb,
                                                          ctx.state->sessions,
                                                          ctx.state->credentials,
                                                          ctx.state->policy,
                                                          ctx.state->rng);
    }
    catch (const std::exception&)
    {
        co_return std::unexpected(make_error_code(errc::protocol_error));
    }

    if (auto err = co_await c->handshake(deadline, std::move(options.stop)); err) co_return std::unexpected(err);
    co_return stream{std::move(c)};
}

coro::task<io::result> stream::read(std::span<std::byte> data)
{
    // as io::reader says: rather than waiting for data that couldn't be taken.
    if (data.empty()) return coro::ready_task<io::result>(io::result{});

    {
        std::scoped_lock lock{conn->mu};

        // usually, there's decrypted data already: then there's no need for a coroutine (see coro::ready_task).
        if (auto count = conn->take_received(data); count > 0)
            return coro::ready_task<io::result>(io::result{.count = count});
        if (conn->failure) return coro::ready_task<io::result>(io::result{.err = conn->failure});
        if (conn->peer_closed)
            return coro::ready_task<io::result>(io::result{.err = make_error_code(io::errc::end_of_stream)});
    }

    return conn->read_more(data);
}

coro::task<io::result> stream::write(std::span<const std::byte> data)
{
    std::uint64_t target = 0;
    {
        std::scoped_lock lock{conn->mu};
        if (conn->failure) return coro::ready_task<io::result>(io::result{.err = conn->failure});
        if (conn->write_failure) return coro::ready_task<io::result>(io::result{.err = conn->write_failure});
        if (conn->channel->is_closed_for_writing())
            return coro::ready_task<io::result>(io::result{.err = make_error_code(io::errc::end_of_stream)});

        try
        {
            conn->channel->send(std::span{reinterpret_cast<const std::uint8_t*>(data.data()), data.size()});
        }
        catch (const std::exception&)
        {
            return coro::ready_task<io::result>(io::result{.err = make_error_code(errc::protocol_error)});
        }

        target = conn->emitted;
    }

    // NOTE: a coroutine of its own (rather than this one), so the lock above is released before it suspends.
    const auto written = data.size();
    return [](detail::connection* conn, std::uint64_t target, std::size_t written) -> coro::task<io::result>
    {
        if (auto err = co_await conn->flush(target, true); err) co_return io::result{.err = err};
        co_return io::result{.count = written};
    }(conn.get(), target, written);
}

coro::task<io::result> stream::wait_writable() { return conn->wait_writable(); }

coro::task<std::error_code> stream::shutdown()
{
    std::uint64_t target = 0;
    {
        std::scoped_lock lock{conn->mu};

        try
        {
            if (!conn->channel->is_closed_for_writing()) conn->channel->close();
        }
        catch (const std::exception&)
        {
            co_return make_error_code(errc::protocol_error);
        }

        target = conn->emitted;
    }

    // what's queued, and close_notify, go first.
    auto err = co_await conn->flush(target, true);
    conn->socket.shutdown_write();
    co_return err;
}

coro::task<> stream::close()
{
    (void)co_await shutdown();
    conn->socket.close(false);
}

std::string_view stream::alpn() const noexcept { return conn->alpn; }

version stream::version() const noexcept { return conn->negotiated; }

bool stream::resumed() const noexcept { return conn->resumed; }

std::shared_ptr<const std::vector<certificate>> stream::peer_certificates() const
{
    std::scoped_lock lock{conn->mu};
    if (conn->peer_certs) return conn->peer_certs;
    if (conn->peer_chain.empty()) return no_certificates();

    auto made = std::make_shared<std::vector<certificate>>();
    made->reserve(conn->peer_chain.size());
    for (const auto& cert : conn->peer_chain) made->push_back(make_certificate(cert));

    conn->peer_certs = std::move(made);
    return conn->peer_certs;
}

void stream::release_close_reply() noexcept
{
    std::scoped_lock lock{conn->mu};
    conn->hold_reply = false;

    try
    {
        // NOTE: as the channel would have, as it read it (but sent with the next write or flush).
        if (conn->peer_closed && !conn->channel->is_closed_for_writing()) conn->channel->close();
    }
    catch (const std::exception&)
    {
        if (!conn->write_failure) conn->write_failure = make_error_code(errc::protocol_error);
    }
}

void stream::set_stop_token(std::stop_token stop) noexcept { conn->socket.set_stop_token(std::move(stop)); }

tcp_socket& stream::socket() noexcept { return conn->socket; }

io::handle stream::native_handle() const noexcept { return conn->socket.native_handle(); }

bool detail::idle_clean(const stream& s) noexcept
{
    auto& c = *s.conn;

    std::scoped_lock lock{c.mu};
    return c.received_start == c.received.size() && !c.peer_closed && !c.failure && !c.write_failure
        && c.header_have == 0 && c.record_left == 0;
}

bool detail::covers(const server_context& ctx, std::string_view host)
{
    // NOTE: Botan's check for a host's name (wildcards as RFC 6125 has them, the common name only without subject
    //       alternative names), which reads an IPv4 or IPv6 address as one, checking the certificate's addresses.
    try
    {
        return ctx.state->leaf.matches_dns_name(host);
    }
    catch (const std::exception&)
    {
        return false;
    }
}

bool detail::verifies(const stream& s) noexcept
{
    const auto& client = s.conn->client;
    return client != nullptr && !client->skip_verification;
}

std::error_code
detail::verify_for(const stream& s, std::string_view host, std::optional<std::chrono::system_clock::time_point> at)
{
    if (!verifies(s) || host.empty()) return std::make_error_code(std::errc::invalid_argument);

    auto& c = *s.conn;

    // NOTE: copies of Botan's certificates share their data. The chain's set by the handshake, but read under mu as
    //       peer_certificates() reads it.
    std::vector<Botan::X509_Certificate> chain;
    {
        std::scoped_lock lock{c.mu};
        chain = c.peer_chain;
    }
    if (chain.empty()) return std::make_error_code(std::errc::invalid_argument);

    try
    {
        // what the handshake's verification has (see Botan's TLS client: its trust store for the host, the server's
        // usage, and its clock), but for host; and no stapled OCSP responses, which aren't kept (the policy doesn't
        // require revocation information, and a handshake with host would have fresh ones).
        const std::string name{host};
        const auto        roots  = c.client->credentials->trusted_certificate_authorities("tls-client", name);
        const auto        result = verify_chain(chain,
                                                {},
                                                roots,
                                                Botan::Usage_Type::TLS_SERVER_AUTH,
                                                name,
                                                *c.client->policy,
                                                at.value_or(c.cb->tls_current_timestamp()),
                                                c.cb->tls_verify_cert_chain_ocsp_timeout());
        if (result.successful_validation()) return {};
        return {static_cast<int>(result.result()), verification_category()};
    }
    catch (const std::exception&)
    {
        // (as the handshake's feed() takes an exception its verification throws.)
        return make_error_code(errc::protocol_error);
    }
}

}
