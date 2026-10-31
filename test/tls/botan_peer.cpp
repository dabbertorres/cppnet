#include "tls/botan_peer.hpp"

// NOTE: before Botan's headers, some of which use <algorithm> without including it (see ports/botan).
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <botan/certstor.h>
#include <botan/credentials_manager.h>
#include <botan/data_src.h>
#include <botan/ocsp.h>
#include <botan/pk_keys.h>
#include <botan/pkcs8.h>
#include <botan/pkix_enums.h>
#include <botan/pkix_types.h>
#include <botan/system_rng.h>
#include <botan/tls_alert.h>
#include <botan/tls_callbacks.h>
#include <botan/tls_channel.h>
#include <botan/tls_client.h>
#include <botan/tls_policy.h>
#include <botan/tls_server.h>
#include <botan/tls_server_info.h>
#include <botan/tls_session.h>
#include <botan/tls_session_manager_noop.h>
#include <botan/tls_version.h>
#include <botan/x509cert.h>

#include "coro/task.hpp"
#include "io/io.hpp"
#include "io/writer.hpp"
#include "tcp.hpp"

namespace test
{

namespace
{

    namespace tls = Botan::TLS;

    // tls12_only is Botan's default policy, but for TLS 1.3: so the peer is Botan's TLS 1.2 implementation, which can
    // renegotiate.
    class tls12_only final : public tls::Policy
    {
    public:
        [[nodiscard]] bool allow_tls13() const override { return false; }
    };

    // credentials is a server's certificate chain and key (none for a client).
    class credentials final : public Botan::Credentials_Manager
    {
    public:
        credentials() = default;

        credentials(std::vector<Botan::X509_Certificate> chain, std::shared_ptr<Botan::Private_Key> key) noexcept
            : chain{std::move(chain)}
            , key{std::move(key)}
        {}

        std::vector<Botan::X509_Certificate> find_cert_chain(const std::vector<std::string>& cert_key_types,
                                                             const std::vector<Botan::AlgorithmIdentifier>& /*schemes*/,
                                                             const std::vector<Botan::X509_DN>& /*acceptable_CAs*/,
                                                             const std::string& type,
                                                             const std::string& /*context*/) override
        {
            if (type != "tls-server" || !key) return {};
            if (!cert_key_types.empty() && !std::ranges::contains(cert_key_types, key->algo_name())) return {};
            return chain;
        }

        std::shared_ptr<Botan::Private_Key> private_key_for(const Botan::X509_Certificate& cert,
                                                            const std::string&             type,
                                                            const std::string& /*context*/) override
        {
            return type == "tls-server" && !chain.empty() && cert == chain.front() ? key : nullptr;
        }

    private:
        std::vector<Botan::X509_Certificate> chain;
        std::shared_ptr<Botan::Private_Key>  key;
    };

    std::vector<Botan::X509_Certificate> load_chain(const std::filesystem::path& path)
    {
        Botan::DataSource_Stream             source{path.string()};
        std::vector<Botan::X509_Certificate> chain;
        try
        {
            while (!source.end_of_data()) chain.emplace_back(source);
        }
        catch (const std::exception&)
        {
            // the text after the last certificate (as cppnet's load_certificates() reads them).
            if (chain.empty()) throw;
        }
        return chain;
    }

    std::shared_ptr<Botan::Private_Key> load_key(const std::filesystem::path& path)
    {
        Botan::DataSource_Stream source{path.string()};
        return Botan::PKCS8::load_key(source);
    }

}

// callbacks buffers what Botan has for the socket (the peer's steps do the I/O), and records what it received in the
// peer.
struct botan_peer::callbacks final : tls::Callbacks
{
    explicit callbacks(botan_peer* peer) noexcept
        : peer{peer}
    {}

    // ciphertext for the socket.
    std::vector<std::uint8_t> to_send;

    void tls_emit_data(std::span<const std::uint8_t> data) override
    {
        to_send.insert(to_send.end(), data.begin(), data.end());
    }

    void tls_record_received(std::uint64_t /*seq_no*/, std::span<const std::uint8_t> data) override
    {
        peer->received.append(reinterpret_cast<const char*>(data.data()), data.size());
    }

    void tls_alert(tls::Alert alert) override
    {
        if (alert.type() == tls::Alert::CloseNotify || peer->alert) return;
        peer->alert = alert_seen{.code = static_cast<std::uint8_t>(alert.type()), .fatal = alert.is_fatal()};
    }

    void tls_session_established(const tls::Session_Summary& session) override
    {
        peer->version = session.version().version_code();
    }

    // NOTE: a client's (see botan_peer::client()).
    void tls_verify_cert_chain(const std::vector<Botan::X509_Certificate>& /*cert_chain*/,
                               const std::vector<std::optional<Botan::OCSP::Response>>& /*ocsp_responses*/,
                               const std::vector<Botan::Certificate_Store*>& /*trusted_roots*/,
                               Botan::Usage_Type /*usage*/,
                               std::string_view /*hostname*/,
                               const tls::Policy& /*policy*/) override
    {}

    botan_peer* peer;
};

botan_peer::botan_peer(net::tcp_socket conn)
    : conn{std::move(conn)}
    , cb{std::make_shared<callbacks>(this)}
{
    this->conn.set_deadline(net::io::clock::now() + std::chrono::seconds{5});
}

botan_peer::~botan_peer() = default;

std::unique_ptr<botan_peer> botan_peer::client(net::tcp_socket conn, const std::vector<std::string>& alpn)
{
    std::unique_ptr<botan_peer> peer{new botan_peer{std::move(conn)}};
    try
    {
        // NOTE: the constructor emits the ClientHello, which handshake() sends.
        peer->channel = std::make_unique<tls::Client>(peer->cb,
                                                      std::make_shared<tls::Session_Manager_Noop>(),
                                                      std::make_shared<credentials>(),
                                                      std::make_shared<tls12_only>(),
                                                      std::make_shared<Botan::System_RNG>(),
                                                      tls::Server_Information{"localhost"},
                                                      tls::Protocol_Version::TLS_V12,
                                                      alpn);
    }
    catch (const std::exception& e)
    {
        peer->failure = std::string{"setup: "} + e.what();
    }
    return peer;
}

std::unique_ptr<botan_peer>
botan_peer::server(net::tcp_socket conn, const std::filesystem::path& chain, const std::filesystem::path& key)
{
    std::unique_ptr<botan_peer> peer{new botan_peer{std::move(conn)}};
    try
    {
        peer->channel = std::make_unique<tls::Server>(peer->cb,
                                                      std::make_shared<tls::Session_Manager_Noop>(),
                                                      std::make_shared<credentials>(load_chain(chain), load_key(key)),
                                                      std::make_shared<tls12_only>(),
                                                      std::make_shared<Botan::System_RNG>());
    }
    catch (const std::exception& e)
    {
        peer->failure = std::string{"setup: "} + e.what();
    }
    return peer;
}

net::coro::task<std::string> botan_peer::flush()
{
    if (cb->to_send.empty()) co_return std::string{};

    // NOTE: taken first, as the write waits (and nothing adds to it meanwhile, as one task uses the peer).
    const auto sending = std::exchange(cb->to_send, {});
    if (auto sent = co_await net::io::write_all(conn, std::as_bytes(std::span{sending})); sent.err)
        co_return "write: " + sent.err.message();
    co_return std::string{};
}

net::coro::task<std::error_code> botan_peer::read_once()
{
    std::array<std::byte, 4'096> buf{};
    auto                         res = co_await conn.read(buf);
    if (res.count > 0)
    {
        try
        {
            channel->received_data({reinterpret_cast<const std::uint8_t*>(buf.data()), res.count});
        }
        catch (const std::exception& e)
        {
            // NOTE: Botan has queued its alert, if it has one, which is still sent.
            if (failure.empty()) failure = e.what();
        }

        if (auto err = co_await flush(); !err.empty() && failure.empty()) failure = err;
    }
    co_return res.err;
}

net::coro::task<std::string> botan_peer::handshake()
{
    if (!failure.empty()) co_return failure;
    if (auto err = co_await flush(); !err.empty()) co_return err;

    while (!channel->is_handshake_complete())
    {
        const auto err = co_await read_once();
        if (!failure.empty()) co_return failure;
        if (err) co_return "read: " + err.message();
    }
    co_return std::string{};
}

net::coro::task<std::string> botan_peer::send(std::string_view data)
{
    if (!failure.empty()) co_return failure;

    try
    {
        channel->send(std::span{reinterpret_cast<const std::uint8_t*>(data.data()), data.size()});
    }
    catch (const std::exception& e)
    {
        co_return std::string{"send: "} + e.what();
    }
    co_return co_await flush();
}

net::coro::task<std::string> botan_peer::receive(std::size_t size)
{
    while (received.size() < size)
    {
        if (!failure.empty()) co_return failure;
        if (alert) co_return "received alert " + std::to_string(alert->code);

        const auto err = co_await read_once();
        if (received.size() >= size) break;
        if (err) co_return "read: " + err.message();
    }
    co_return std::string{};
}

net::coro::task<std::string> botan_peer::renegotiate()
{
    if (!failure.empty()) co_return failure;

    try
    {
        channel->renegotiate();
    }
    catch (const std::exception& e)
    {
        co_return std::string{"renegotiate: "} + e.what();
    }
    co_return co_await flush();
}

net::coro::task<std::error_code> botan_peer::read_to_end()
{
    while (true)
    {
        if (auto err = co_await read_once(); err) co_return err;
    }
}

}
