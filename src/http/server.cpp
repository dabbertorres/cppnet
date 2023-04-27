#include "http/server.hpp"

#include <functional>

#include "http/http11.hpp"
#include "http/http2.hpp"
#include "http/request.hpp"
#include "io/buffered_writer.hpp"
#include "util/thread_pool.hpp"

namespace net::http
{

server::server(const server_config& cfg)
    : listener(cfg.host,
               cfg.port,
               network::tcp,
               protocol::not_care,
               std::chrono::duration_cast<std::chrono::microseconds>(cfg.header_read_timeout))
    , is_serving{false}
    , router(cfg.router)
    , logger{cfg.logger}
    , threads(cfg.num_threads)
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
            auto client_sock = std::make_shared<tcp_socket>(listener.accept());
            logger->trace("connection accepted: {} -> {}", client_sock->remote_addr(), client_sock->local_addr());

            const std::lock_guard guard(connections_mu);
            /* connections.push_back(client_sock); */
            connections.emplace_back([this, client_sock] { serve_connection(*client_sock); });
        }
        catch (const std::exception& ex)
        {
            // TODO
            logger->critical("exception", ex.what());
        }
    }
}

void server::serve_connection(tcp_socket& client_sock) noexcept
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
            switch (1) // TODO: determine version of HTTP before full decoding
            {
            case 1: decode = http11::request_decode; break;
            case 2: decode = http2::request_decode; break;

            default:
                [[unlikely]]
#ifdef __cpp_lib_unreachable
                std::unreachable()
#endif
                    ;
            }

            auto req_result = decode(reader, max_header_bytes);
            if (req_result.has_error())
            {
                logger->error("request decode error: {}", req_result.to_error().message());
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
            else
            {
                auto maybe_handler = router.route_request(req);
                if (maybe_handler.has_value())
                {
                    auto handler = maybe_handler.value();
                    logger->trace("calling handler");
                    std::invoke(handler.get(), req, rw);
                }
                else
                {
                    logger->trace("no handler found for {} {} - responding 404",
                                  method_string(req.method),
                                  req.uri.path);
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
