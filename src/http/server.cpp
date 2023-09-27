#include "http/server.hpp"

#include <functional>
#include <string_view>

#include "coro/thread_pool.hpp"
#include "http/http11.hpp"
#include "http/http2.hpp"
#include "http/request.hpp"
#include "io/buffered_writer.hpp"

namespace net::http
{

using namespace std::string_view_literals;

server::server(router&& handler, const server_config& cfg)
    : listener{cfg.host,
               cfg.port,
               network::tcp,
               protocol::not_care,
               std::chrono::duration_cast<std::chrono::microseconds>(cfg.header_read_timeout)}
    , is_serving{false}
    , handler{handler}
    , logger{cfg.logger}
    , threads{cfg.num_threads}
    , max_header_bytes{cfg.max_header_bytes}
    , max_pending_connections{cfg.max_pending_connections}
{}

server::~server() { close(); }

void server::close()
{
    is_serving = false;
    is_serving.notify_all();
    logger->flush();
}

void server::serve()
{
    if (is_serving.exchange(true))
    {
        throw std::runtime_error("already serving");
    }

    listener.listen(max_pending_connections);

    while (is_serving)
    {
        // TODO: pull from ready sockets via coroutines

        try
        {
            logger->trace("waiting for connection");
            auto client_sock = listener.accept();
            logger->trace("connection accepted: {} -> {}", client_sock.remote_addr(), client_sock.local_addr());

            std::lock_guard guard{connections_mu};
            connections.emplace_back([this, client_sock = std::move(client_sock)]() mutable
                                     { serve_connection(std::move(client_sock)); });
        }
        catch (const std::exception& ex)
        {
            // TODO
            logger->critical("exception", ex.what());
        }
    }
}

void server::serve_connection(tcp_socket&& client_sock) noexcept
{
    logger->trace("new connection handler started for {} -> {}", client_sock.remote_addr(), client_sock.local_addr());

    io::buffered_reader reader(&client_sock);
    io::buffered_writer writer(&client_sock);

    try
    {
        while (is_serving && client_sock.valid())
        {
            /* threads.schedule( */
            /*     [&] */
            /*     { */
            logger->trace("decoding request");

            request_decoder decode = nullptr;
            switch (0) // TODO: determine version of HTTP before full decoding
            {
            case 1: decode = http11::request_decode; break;
            case 2: decode = http2::request_decode; break;

            default: decode = http11::request_decode; break;
            }

            auto req_result = decode(reader, max_header_bytes);
            if (req_result.has_error())
            {
                auto err = req_result.to_error();
                if (static_cast<io::status_condition>(err.value()) == io::status_condition::closed)
                    logger->debug("connection closed");
                else logger->error("request decode error: {} '{}'", err.value(), err.message());
                break;
            }

            logger->trace("request decoded");

            auto req = req_result.to_value();
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
                .body    = &writer,
            };

            response_writer rw(&writer, &resp, encode);

            if (unsupported)
            {
                logger->trace("unsupported http version: {}.{}", req.version.major, req.version.minor);
                rw.send(status::HTTP_VERSION_NOT_SUPPORTED, 0);
            }
            else if (auto upgrade_to = upgrade_to_protocol(req); !upgrade_to.empty())
            {
                logger->trace("upgrading to protocol: {}", upgrade_to);
                resp.headers.set("Upgrade"sv, upgrade_to);
                resp.headers.set("Connection"sv, "upgrade"sv);
                rw.send(status::SWITCHING_PROTOCOLS, 0);
            }
            else
            {
                auto maybe_handler = handler.route_request(req);
                if (maybe_handler.has_value())
                {
                    auto this_handler = maybe_handler.value();
                    logger->trace("calling handler");
                    std::invoke(this_handler.get(), req, rw);
                }
                else
                {
                    logger->trace("no handler found for {} {} - responding 404",
                                  method_string(req.method),
                                  req.uri.path);
                    // TODO: add configuration for a 404 handler
                    rw.send(status::NOT_FOUND, 0);
                }
            }

            logger->trace("flushing writer");
            writer.flush();
            logger->trace("response sent");
            /* }); */
        }
    }
    catch (const std::exception& ex)
    {
        logger->error("fatal exception in connection handler: {}", ex.what());
    }

    logger->trace("connection closing");
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
