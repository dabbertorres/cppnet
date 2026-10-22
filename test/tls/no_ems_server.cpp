#include "tls/no_ems_server.hpp"

// NOTE: before Botan's headers, some of which use <algorithm> without including it (see ports/botan).
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <botan/credentials_manager.h>
#include <botan/data_src.h>
#include <botan/pk_keys.h>
#include <botan/pkcs8.h>
#include <botan/pkix_types.h>
#include <botan/system_rng.h>
#include <botan/tls_alert.h>
#include <botan/tls_callbacks.h>
#include <botan/tls_extensions.h>
#include <botan/tls_magic.h>
#include <botan/tls_policy.h>
#include <botan/tls_server.h>
#include <botan/tls_session.h>
#include <botan/tls_session_manager_noop.h>
#include <botan/tls_version.h>
#include <botan/x509cert.h>

#include "coro/task.hpp"
#include "io/io.hpp"
#include "io/writer.hpp"
#include "listen.hpp"

namespace test
{

namespace
{

    namespace tls = Botan::TLS;

    // tls12_only is Botan's default policy, but for TLS 1.3: so the server is Botan's TLS 1.2 implementation (without
    // TLS 1.3's downgrade sentinel in its random, which a client offering 1.3 would refuse).
    class tls12_only final : public tls::Policy
    {
    public:
        [[nodiscard]] bool allow_tls13() const override { return false; }
    };

    // credentials is the server's certificate chain and key.
    class credentials final : public Botan::Credentials_Manager
    {
    public:
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
            if (type != "tls-server") return {};
            if (!cert_key_types.empty() && !std::ranges::contains(cert_key_types, key->algo_name())) return {};
            return chain;
        }

        std::shared_ptr<Botan::Private_Key> private_key_for(const Botan::X509_Certificate& cert,
                                                            const std::string&             type,
                                                            const std::string& /*context*/) override
        {
            return type == "tls-server" && cert == chain.front() ? key : nullptr;
        }

    private:
        std::vector<Botan::X509_Certificate> chain;
        std::shared_ptr<Botan::Private_Key>  key;
    };

    // callbacks buffers what Botan has for the socket and the application (serve_without_ems() does the I/O), and
    // leaves Extended Master Secret out of the ServerHello, recording what happens in out.
    class callbacks final : public tls::Callbacks
    {
    public:
        explicit callbacks(no_ems_outcome* out) noexcept
            : out{out}
        {}

        // ciphertext for the socket, and plaintext from the client.
        std::vector<std::uint8_t> to_send;
        std::vector<std::uint8_t> received;

        void tls_emit_data(std::span<const std::uint8_t> data) override
        {
            to_send.insert(to_send.end(), data.begin(), data.end());
        }

        void tls_record_received(std::uint64_t /*seq_no*/, std::span<const std::uint8_t> data) override
        {
            received.insert(received.end(), data.begin(), data.end());
        }

        void tls_alert(tls::Alert alert) override
        {
            if (alert.type() != tls::Alert::CloseNotify) out->client_alert = static_cast<std::uint8_t>(alert.type());
        }

        // NOTE: Botan derives the master secret, and records the session's use of the extension (which this reports),
        //       from the ServerHello as sent, after this has changed it.
        void tls_modify_extensions(tls::Extensions&     extensions,
                                   tls::Connection_Side side,
                                   tls::Handshake_Type  message) override
        {
            if (side == tls::Connection_Side::Server && message == tls::Handshake_Type::ServerHello)
                extensions.remove_extension(tls::Extension_Code::ExtendedMasterSecret);
        }

        void tls_examine_extensions(const tls::Extensions& extensions,
                                    tls::Connection_Side   side,
                                    tls::Handshake_Type    message) override
        {
            if (side == tls::Connection_Side::Client && message == tls::Handshake_Type::ClientHello)
                out->client_offered_ems = extensions.has(tls::Extension_Code::ExtendedMasterSecret);
        }

        void tls_session_established(const tls::Session_Summary& session) override
        {
            out->handshake_done = true;
            out->used_ems       = session.supports_extended_master_secret();
            out->version        = session.version().version_code();
        }

    private:
        no_ems_outcome* out;
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

net::coro::task<>
serve_without_ems(net::listener* listener, std::filesystem::path chain, std::filesystem::path key, no_ems_outcome* out)
{
    auto accepted = co_await listener->accept();
    if (!accepted.has_value())
    {
        out->failure = "accept: " + accepted.error().message();
        co_return;
    }

    auto& conn = *accepted;
    conn.set_deadline(net::io::clock::now() + std::chrono::seconds{5});

    auto                         cb = std::make_shared<callbacks>(out);
    std::unique_ptr<tls::Server> server;
    try
    {
        server = std::make_unique<tls::Server>(cb,
                                               std::make_shared<tls::Session_Manager_Noop>(),
                                               std::make_shared<credentials>(load_chain(chain), load_key(key)),
                                               std::make_shared<tls12_only>(),
                                               std::make_shared<Botan::System_RNG>());
    }
    catch (const std::exception& e)
    {
        out->failure = std::string{"setup: "} + e.what();
        co_return;
    }

    std::array<std::byte, 4'096> buf{};
    while (!server->is_closed())
    {
        auto res = co_await conn.read(buf);
        if (res.count > 0)
        {
            try
            {
                server->received_data({reinterpret_cast<const std::uint8_t*>(buf.data()), res.count});

                // echo what the client sent.
                if (!cb->received.empty() && server->is_active())
                {
                    out->echoed.append(reinterpret_cast<const char*>(cb->received.data()), cb->received.size());
                    server->send(cb->received);
                    cb->received.clear();
                }
            }
            catch (const std::exception& e)
            {
                // NOTE: Botan has queued its alert, if it has one, which is still sent.
                out->failure = e.what();
            }
        }

        // NOTE: what Botan has to send is taken first, as the write waits (and nothing adds to it meanwhile, as this
        //       coroutine is the only one using the server).
        if (!cb->to_send.empty())
        {
            const auto sending = std::exchange(cb->to_send, {});
            if (auto sent = co_await net::io::write_all(conn, std::as_bytes(std::span{sending})); sent.err)
            {
                if (out->failure.empty()) out->failure = "write: " + sent.err.message();
                co_return;
            }
        }

        if (!out->failure.empty()) co_return;
        if (res.err)
        {
            // the client closing the connection after it failed the handshake, or after its close_notify, is how it
            // ends: anything else is a failure.
            if (!server->is_closed() && !out->client_alert) out->failure = "read: " + res.err.message();
            co_return;
        }
    }
}

}
