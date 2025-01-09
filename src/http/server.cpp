#include "http/server.hpp"

#include <atomic>
#include <chrono>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "coro/task.hpp"
#include "exception.hpp"
#include "http/http11.hpp"
#include "http/http2.hpp"
#include "http/request.hpp"
#include "http/response.hpp"
#include "http/router.hpp"
#include "io/buffered_reader.hpp"
#include "io/buffered_writer.hpp"
#include "io/io.hpp"
#include "io/scheduler.hpp"
#include "ip_addr.hpp"
#include "listen.hpp"
#include "tcp.hpp"

namespace net::http
{

using namespace std::string_view_literals;

server::server(io::scheduler* scheduler, router&& handler, const server_config& cfg)
    : listener{scheduler,
               cfg.host,
               cfg.port,
               network::tcp,
               protocol::not_care,
               std::chrono::duration_cast<std::chrono::microseconds>(cfg.header_read_timeout)}
    , is_serving{false}
    , handler{std::move(handler)}
    , logger{cfg.logger}
    , scheduler{scheduler}
    , max_header_bytes{cfg.max_header_bytes}
    , max_pending_connections{cfg.max_pending_connections}
{}

server::~server() { close(); }

void server::close()
{
    if (is_serving.exchange(false, std::memory_order::acq_rel))
    {
        listener.shutdown();
        logger->flush();
    }
}

coro::task<> server::serve()
{
    if (is_serving.exchange(true, std::memory_order::acq_rel))
    {
        throw exception{"already serving"};
    }

    listener.listen(max_pending_connections);

    while (is_serving.load(std::memory_order::acquire))
    {
        try
        {
            logger->trace("waiting for connection");
            auto client_sock = co_await listener.accept();
            logger->trace("connection accepted: {} -> {}", client_sock.remote_addr(), client_sock.local_addr());
            if (!client_sock.valid())
            {
                if (!is_serving.load(std::memory_order::acquire)) co_return;
                else throw std::runtime_error{"listener unexpected closed"};
            }

            scheduler->schedule(serve_connection(std::move(client_sock)));
        }
        catch (const std::exception& ex)
        {
            logger->error("exception: {}", ex.what());
        }
    }
}

coro::task<> server::serve_connection(tcp_socket conn) noexcept
{
    try
    {
        while (is_serving.load(std::memory_order::acquire) && conn.valid())
        {
            logger->trace("decoding request");

            request_decoder decode = nullptr;
            switch (0) // TODO: determine version of HTTP before full decoding
            {
            case 1: decode = http11::request_decode; break;
            case 2: decode = http2::request_decode; break;

            default: decode = http11::request_decode; break;
            }

            auto reader = std::make_unique<io::buffered_reader>(&conn);
            auto writer = std::make_unique<io::buffered_writer>(&conn);

            auto req_result = co_await decode(std::move(reader), max_header_bytes);
            if (!req_result.has_value())
            {
                auto err = req_result.error();
                if (static_cast<io::status_condition>(err.value()) == io::status_condition::closed)
                {
                    logger->debug("connection closed");
                }
                else
                {
                    logger->debug("request decoding error: {} '{}'", err.value(), err.message());
                }
                break;
            }

            logger->trace("request decoded");

            auto& req = req_result.value();
            logger->trace("request {} {} as HTTP/{}.{}",
                          method_string(req.method),
                          req.uri.path,
                          req.version.major,
                          req.version.minor);

            bool             unsupported = false;
            response_encoder encode      = nullptr;
            switch (req.version.major)
            {
            case 1: encode = http11::response_encode; break;
            case 2: encode = http2::response_encode; break;
            default:
                [[unlikely]] unsupported = true;
                encode                   = http11::response_encode;
                break;
            }

            server_response resp{
                .version = req.version,
                .body    = writer.get(),
            };

            response_writer rw{writer.get(), &resp, encode};

            if (unsupported)
            {
                logger->trace("unsupported http version: {}.{}", req.version.major, req.version.minor);
                co_await rw.send(status::HTTP_VERSION_NOT_SUPPORTED, 0);
            }
            else if (auto upgrade_to = upgrade_to_protocol(req); !upgrade_to.empty())
            {
                logger->trace("upgrading to protocol: {}", upgrade_to);
                resp.headers.set("Upgrade"sv, upgrade_to);
                resp.headers.set("Connection"sv, "upgrade"sv);
                co_await rw.send(status::SWITCHING_PROTOCOLS, 0);
            }
            else
            {
                logger->trace("calling handler");
                co_await handler(req, rw);
            }

            logger->trace("flushing writer");
            co_await writer->flush();
            logger->trace("response sent");
        }
    }
    catch (const std::exception& ex)
    {
        logger->error("fatal exception in connection handler: {}", ex.what());
    }

    logger->trace("connection closing");
}

void server::serve_http11(tcp_socket conn) noexcept
{
    while (is_serving && conn.valid())
    {}
}

void server::serve_http2(tcp_socket conn) noexcept
{
    while (is_serving && conn.valid())
    {}
}

std::string_view server::upgrade_to_protocol(const server_request& req) const noexcept
{
    auto upgrade = req.headers.get_all("Upgrade"sv);
    if (!upgrade.has_value()) return ""sv;

    std::string_view upgrade_to;
    for (const auto& protocol : *upgrade)
    {
        if (is_protocol_supported(protocol))
        {
            upgrade_to = protocol;
            break;
        }
    }

    return upgrade_to;
}

bool server::is_protocol_supported(std::string_view protocol) const noexcept
{
    if (protocol == "HTTP/1.1") return true;

    return false;
}

bool server::enforce_protocol(const server_request& req, response_writer& resp) noexcept
{
    // TODO:
    // Field values containing CR, LF, or NUL characters are invalid and dangerous, due to the
    // varying ways that implementations might parse and interpret those characters; a recipient
    // of CR, LF, or NUL within a field value MUST either reject the message or replace each of
    // those characters with SP before further processing or forwarding of that message.
    // Field values containing other CTL characters are also invalid; however, recipients MAY
    // retain such characters for the sake of robustness when they appear within a safe context
    // (e.g., an application-specific quoted string that will not be processed by any downstream HTTP parser).

    // TODO: delimiters around commas in "list-based fields"
}

}
