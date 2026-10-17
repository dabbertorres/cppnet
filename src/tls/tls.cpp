#include "tls/tls.hpp"

// NOTE: before Botan's headers, some of which use <algorithm> without including it (see ports/botan).
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <botan/certstor.h>
#include <botan/certstor_system.h>
#include <botan/credentials_manager.h>
#include <botan/data_src.h>
#include <botan/pkcs8.h>
#include <botan/system_rng.h>
#include <botan/tls_alert.h>
#include <botan/tls_callbacks.h>
#include <botan/tls_channel.h>
#include <botan/tls_client.h>
#include <botan/tls_exceptn.h>
#include <botan/tls_policy.h>
#include <botan/tls_server.h>
#include <botan/tls_server_info.h>
#include <botan/tls_session_manager_memory.h>
#include <botan/tls_session_manager_noop.h>
#include <botan/tls_version.h>
#include <botan/x509cert.h>

#include "coro/task.hpp"
#include "io/io.hpp"
#include "io/writer.hpp"
#include "tcp.hpp"
#include "util/defer.hpp"

namespace
{

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

        if (code >= 0 && code <= 255) return Botan::TLS::Alert(static_cast<Botan::TLS::Alert::Type>(code)).type_string();
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

std::error_code alert_code(Botan::TLS::Alert::Type type) noexcept
{
    return {static_cast<int>(type), net::tls::category()};
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

// policy is Botan's default TLS policy, except that it doesn't require revocation information (OCSP or CRLs) for the
// peer's certificates: many servers have none to give (e.g. Let's Encrypt's, which stopped OCSP in 2025), and neither
// browsers, Go, nor OpenSSL require it by default.
class policy final : public Botan::TLS::Policy
{
public:
    [[nodiscard]] bool require_cert_revocation_info() const override { return false; }
};

// client_credentials gives a client the certificate authorities it trusts.
class client_credentials final : public Botan::Credentials_Manager
{
public:
    explicit client_credentials(std::unique_ptr<Botan::Certificate_Store> store) noexcept
        : store{std::move(store)}
    {}

    std::vector<Botan::Certificate_Store*> trusted_certificate_authorities(const std::string& type,
                                                                           const std::string& /*context*/) override
    {
        if (type == "tls-client") return {store.get()};
        return {};
    }

private:
    std::unique_ptr<Botan::Certificate_Store> store;
};

// server_credentials gives a server its certificate chain and key.
class server_credentials final : public Botan::Credentials_Manager
{
public:
    server_credentials(std::vector<Botan::X509_Certificate> chain, std::shared_ptr<Botan::Private_Key> key) noexcept
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

        // the client must accept the key's kind (e.g. ECDSA, RSA).
        if (!cert_key_types.empty() && std::ranges::find(cert_key_types, key->algo_name()) == cert_key_types.end())
            return {};

        return chain;
    }

    std::shared_ptr<Botan::Private_Key> private_key_for(const Botan::X509_Certificate& cert,
                                                        const std::string& /*type*/,
                                                        const std::string& /*context*/) override
    {
        return cert == chain.front() ? key : nullptr;
    }

private:
    std::vector<Botan::X509_Certificate> chain;
    std::shared_ptr<Botan::Private_Key>  key;
};

}

namespace net::tls::detail
{

struct client_state
{
    std::shared_ptr<Botan::RandomNumberGenerator>        rng;
    std::shared_ptr<Botan::Credentials_Manager>          credentials;
    std::shared_ptr<Botan::TLS::Session_Manager>         sessions;
    std::shared_ptr<const Botan::TLS::Policy>            policy;
    bool                                                 skip_verification = false;
    std::vector<std::string>                             alpn;
};

struct server_state
{
    std::shared_ptr<Botan::RandomNumberGenerator> rng;
    std::shared_ptr<Botan::Credentials_Manager>   credentials;
    std::shared_ptr<Botan::TLS::Session_Manager>  sessions;
    std::shared_ptr<const Botan::TLS::Policy>     policy;
    std::vector<std::string>                      alpn;
};

struct connection;

// callbacks are Botan's calls back into a connection. They're only made while the connection's mutex is held (from
// inside the channel's methods, which are only called holding it), and only touch what it guards.
// NOLINTNEXTLINE(misc-use-internal-linkage): a member of connection, which the header declares (so mustn't be internal).
class callbacks final : public Botan::TLS::Callbacks
{
public:
    callbacks(connection* conn, bool skip_verification, std::vector<std::string> server_alpn) noexcept
        : conn{conn}
        , skip_verification{skip_verification}
        , server_alpn{std::move(server_alpn)}
    {}

    void tls_emit_data(std::span<const std::uint8_t> data) override;
    void tls_record_received(std::uint64_t seq_no, std::span<const std::uint8_t> data) override;
    void tls_alert(Botan::TLS::Alert alert) override;

    void tls_verify_cert_chain(const std::vector<Botan::X509_Certificate>&                cert_chain,
                               const std::vector<std::optional<Botan::OCSP::Response>>&   ocsp_responses,
                               const std::vector<Botan::Certificate_Store*>&              trusted_roots,
                               Botan::Usage_Type                                          usage,
                               std::string_view                                           hostname,
                               const Botan::TLS::Policy&                                  policy) override
    {
        if (skip_verification) return;
        Callbacks::tls_verify_cert_chain(cert_chain, ocsp_responses, trusted_roots, usage, hostname, policy);
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
    connection*              conn;
    bool                     skip_verification;
    std::vector<std::string> server_alpn;
};

struct connection
{
    explicit connection(tcp_socket conn) noexcept
        : socket{std::move(conn)}
    {}

    // take_received moves what's been decrypted (and not read yet) into data, returning how much. Must be called holding
    // mu.
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
        try
        {
            channel->received_data(ciphertext);
        }
        catch (const Botan::TLS::TLS_Exception& ex)
        {
            if (!failure) failure = alert_code(ex.type());
        }
        catch (const std::exception&)
        {
            if (!failure) failure = make_error_code(errc::protocol_error);
        }
    }

    // flush writes what the channel has emitted (handshake messages, records, alerts) to the socket. If another call
    // is flushing already, that one writes it (so writes go out in the order they were emitted), and this returns.
    coro::task<std::error_code> flush()
    {
        std::vector<std::uint8_t> pending;

        {
            std::scoped_lock lock{mu};
            if (flushing || outgoing.empty()) co_return std::error_code{};

            flushing = true;
            pending.swap(outgoing);
        }

        while (true)
        {
            auto res = co_await io::write_all(socket, std::as_bytes(std::span{pending}));

            std::scoped_lock lock{mu};
            if (res.err)
            {
                flushing = false;
                co_return res.err;
            }

            pending.clear();
            if (outgoing.empty())
            {
                flushing = false;
                co_return std::error_code{};
            }

            pending.swap(outgoing);
        }
    }

    // receive reads from the socket once, and passes what it read to the channel, returning the read's error (if any).
    coro::task<std::error_code> receive()
    {
        // NOTE: only one read waits at once (as for the socket), so ciphertext isn't shared.
        auto res = co_await socket.read(std::as_writable_bytes(std::span{ciphertext}));

        if (res.count > 0)
        {
            {
                std::scoped_lock lock{mu};
                feed(std::span{ciphertext}.first(res.count));
            }

            // e.g. handshake messages, or an alert, in reply.
            if (auto err = co_await flush(); err && !res.err) res.err = err;
        }

        co_return res.err;
    }

    // handshake runs the handshake until it's done, deadline passes, or it fails.
    coro::task<std::error_code> handshake(io::clock::time_point deadline)
    {
        socket.set_deadline(deadline);
        util::defer no_deadline{[this] noexcept { socket.set_deadline(std::nullopt); }};

        // e.g. the client's hello.
        if (auto err = co_await flush(); err) co_return err;

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
    std::vector<std::uint8_t> outgoing; // ciphertext to write
    std::vector<std::uint8_t> received; // decrypted data not read yet, from received_start
    std::size_t               received_start = 0;
    bool                      peer_closed    = false; // the peer's close_notify
    std::error_code           failure;                // a fatal alert, or a broken protocol
    bool                      flushing = false;
    std::string               alpn;

    // what a read of the socket reads into: only one waits at once.
    std::array<std::uint8_t, (std::size_t{16} * 1'024) + 1'024> ciphertext{};

    std::shared_ptr<callbacks>          cb;
    std::unique_ptr<Botan::TLS::Channel> channel;
};

void callbacks::tls_emit_data(std::span<const std::uint8_t> data)
{
    conn->outgoing.insert(conn->outgoing.end(), data.begin(), data.end());
}

void callbacks::tls_record_received(std::uint64_t /*seq_no*/, std::span<const std::uint8_t> data)
{
    conn->received.insert(conn->received.end(), data.begin(), data.end());
}

void callbacks::tls_alert(Botan::TLS::Alert alert)
{
    if (alert.type() == Botan::TLS::Alert::CloseNotify) conn->peer_closed = true;
    else if (alert.is_fatal() && !conn->failure) conn->failure = alert_code(alert.type());
}

}

namespace net::tls
{

const std::error_category& category() noexcept
{
    static const tls_category_impl instance;
    return instance;
}

std::error_code make_error_code(errc err) noexcept { return {static_cast<int>(err), category()}; }

client_context::client_context(std::shared_ptr<detail::client_state> state) noexcept
    : state{std::move(state)}
{}

std::expected<client_context, std::error_code> client_context::make(const client_options& options)
{
    try
    {
        std::unique_ptr<Botan::Certificate_Store> store;
        if (options.trusted_ca_file)
        {
            auto certs = load_certificates(*options.trusted_ca_file);
            if (!certs.has_value()) return std::unexpected(certs.error());

            auto in_memory = std::make_unique<Botan::Certificate_Store_In_Memory>();
            for (const auto& cert : *certs) in_memory->add_certificate(cert);
            store = std::move(in_memory);
        }
        else
        {
            store = std::make_unique<Botan::System_Certificate_Store>();
        }

        auto rng   = std::make_shared<Botan::System_RNG>();
        auto state = std::make_shared<detail::client_state>(detail::client_state{
            .rng               = rng,
            .credentials       = std::make_shared<client_credentials>(std::move(store)),
            .sessions          = std::make_shared<Botan::TLS::Session_Manager_In_Memory>(rng),
            .policy            = std::make_shared<policy>(),
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
    try
    {
        auto chain = load_certificates(options.certificate_chain);
        if (!chain.has_value()) return std::unexpected(chain.error());

        auto key_source = open_file(options.private_key);
        if (!key_source.has_value()) return std::unexpected(key_source.error());

        std::shared_ptr<Botan::Private_Key> key = Botan::PKCS8::load_key(**key_source);
        if (!key) return std::unexpected(std::make_error_code(std::errc::invalid_argument));

        auto rng   = std::make_shared<Botan::System_RNG>();
        auto state = std::make_shared<detail::server_state>(detail::server_state{
            .rng         = rng,
            .credentials = std::make_shared<server_credentials>(std::move(*chain), std::move(key)),
            .sessions    = std::make_shared<Botan::TLS::Session_Manager_Noop>(),
            .policy      = std::make_shared<policy>(),
            .alpn        = options.alpn,
        });

        return server_context{std::move(state)};
    }
    catch (const std::exception&)
    {
        // e.g. the key isn't PKCS #8.
        return std::unexpected(std::make_error_code(std::errc::invalid_argument));
    }
}

stream::stream(std::unique_ptr<detail::connection> conn) noexcept
    : conn{std::move(conn)}
{}

stream::stream(stream&&) noexcept            = default;
stream& stream::operator=(stream&&) noexcept = default;
stream::~stream()                            = default;

coro::task<std::expected<stream, std::error_code>>
stream::connect(tcp_socket conn, const client_context& ctx, std::string host, io::clock::time_point deadline)
{
    // the certificate is checked against host: without one, nothing would be.
    if (host.empty() && !ctx.state->skip_verification)
        co_return std::unexpected(std::make_error_code(std::errc::invalid_argument));

    auto c = std::make_unique<detail::connection>(std::move(conn));

    try
    {
        c->cb = std::make_shared<detail::callbacks>(c.get(), ctx.state->skip_verification, std::vector<std::string>{});

        std::scoped_lock lock{c->mu};
        c->channel = std::make_unique<Botan::TLS::Client>(c->cb,
                                                          ctx.state->sessions,
                                                          ctx.state->credentials,
                                                          ctx.state->policy,
                                                          ctx.state->rng,
                                                          Botan::TLS::Server_Information{host},
                                                          Botan::TLS::Protocol_Version::latest_tls_version(),
                                                          ctx.state->alpn);
    }
    catch (const std::exception&)
    {
        co_return std::unexpected(make_error_code(errc::protocol_error));
    }

    if (auto err = co_await c->handshake(deadline); err) co_return std::unexpected(err);
    co_return stream{std::move(c)};
}

coro::task<std::expected<stream, std::error_code>>
stream::accept(tcp_socket conn, const server_context& ctx, io::clock::time_point deadline)
{
    auto c = std::make_unique<detail::connection>(std::move(conn));

    try
    {
        c->cb = std::make_shared<detail::callbacks>(c.get(), false, ctx.state->alpn);

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

    if (auto err = co_await c->handshake(deadline); err) co_return std::unexpected(err);
    co_return stream{std::move(c)};
}

coro::task<io::result> stream::read(std::span<std::byte> data)
{
    {
        std::scoped_lock lock{conn->mu};

        // usually, there's decrypted data already: then there's no need for a coroutine (see coro::ready_task).
        if (auto count = conn->take_received(data); count > 0) return coro::ready_task<io::result>(io::result{.count = count});
        if (conn->failure) return coro::ready_task<io::result>(io::result{.err = conn->failure});
        if (conn->peer_closed)
            return coro::ready_task<io::result>(io::result{.err = make_error_code(io::errc::end_of_stream)});
    }

    return conn->read_more(data);
}

coro::task<io::result> stream::write(std::span<const std::byte> data)
{
    {
        std::scoped_lock lock{conn->mu};
        if (conn->failure) return coro::ready_task<io::result>(io::result{.err = conn->failure});
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
    }

    // NOTE: a coroutine of its own (rather than this one), so the lock above is released before it suspends.
    const auto written = data.size();
    return [](detail::connection* conn, std::size_t written) -> coro::task<io::result>
    {
        if (auto err = co_await conn->flush(); err) co_return io::result{.err = err};
        co_return io::result{.count = written};
    }(conn.get(), written);
}

coro::task<> stream::close()
{
    {
        std::scoped_lock lock{conn->mu};
        if (!conn->channel->is_closed()) conn->channel->close();
    }

    (void)co_await conn->flush();
    conn->socket.close(false);
}

std::string_view stream::alpn() const noexcept { return conn->alpn; }

tcp_socket& stream::socket() noexcept { return conn->socket; }

int stream::native_handle() const noexcept { return conn->socket.native_handle(); }

}
