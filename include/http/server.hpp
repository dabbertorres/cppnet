#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

#include <spdlog/logger.h>
#include <spdlog/spdlog.h>

#include "http/handler.hpp"
#include "http/http.hpp"
#include "http/http11.hpp"
#include "http/http2.hpp"
#include "io/buffered_reader.hpp"
#include "io/buffered_writer.hpp"

#include "listen.hpp"

namespace net::http
{

// NOTE: clangd seems to think Handler isn't being used - so make it explicit.
using net::http::Handler;

struct server_config
{
    std::string                     host                    = "localhost";
    std::string                     port                    = "8080";
    size_t                          max_header_bytes        = 8192;
    std::chrono::seconds            header_read_timeout     = 5s;
    uint16_t                        max_pending_connections = 512;
    std::shared_ptr<spdlog::logger> logger                  = spdlog::default_logger();
    bool                            http11                  = true;
    bool                            http2                   = false;
    bool                            http3                   = false;
    // TODO: threading
    // TODO: coroutines
};

class server
{
public:
    server(const server_config& cfg = server_config{});

    server(const server&)            = delete;
    server& operator=(const server&) = delete;

    server(server&&) noexcept;
    server& operator=(server&&) noexcept;

    ~server();

    void close();

    template<Handler H>
    void serve(H&& handler)
    {
        if (is_serving.exchange(true))
        {
            // TODO: throw "already serving"
            throw std::runtime_error("already serving");
        }

        listener.listen(max_pending_connections);

        while (is_serving)
        {
            // TODO: thread pool and coroutines

            try
            {
                logger->trace("waiting for connection");
                auto client_sock = listener.accept();
                logger->trace("connection accepted: {} -> {}", client_sock.remote_addr(), client_sock.local_addr());

                {
                    const std::lock_guard guard(connections_mu);
                    connections.emplace_back(
                        [this, &handler](tcp_socket&& socket)
                        {
                            logger->trace("new connection handler started");

                            // TODO: pull from a queue when connection closes
                            // TODO: pull from ready sockets via coroutines

                            io::buffered_reader reader(socket, max_header_bytes);

                            logger->trace("ready to read from connection");

                            io::buffered_writer<char> writer(socket);

                            logger->trace("ready to write to connection");

                            try
                            {
                                while (is_serving && socket.valid())
                                {
                                    logger->trace("decoding request");

                                    auto req_result = http11::request_decode(reader);
                                    /* TODO http2::request_decode(client_sock); */

                                    logger->trace("request decoded");
                                    if (req_result.has_error())
                                    {
                                        logger->error("request decode error: {}", req_result.to_error().message());
                                        break;
                                    }

                                    auto req = req_result.to_value();

                                    server_response resp{
                                        .version = req.version,
                                        .body    = writer,
                                    };

                                    logger->trace("calling handler");
                                    handler(req, resp);
                                    logger->trace("handler returned");

                                    // TODO: this needs to start happening before the handler
                                    // e.g. large responses should be streamed, rather than
                                    // written to memory, and then written again.
                                    //
                                    // Options:
                                    //   * coroutine?
                                    //   * server_response interface only exposes methods for
                                    //     setting response code, headers, writing the body, etc
                                    /* logger->trace("flushing writer"); */
                                    /* resp.body->flush(); */
                                    logger->trace("encoding response");

                                    if (auto err = http11::response_encode(writer, resp); err)
                                    {
                                        logger->error("response encoding: {}", err.message());
                                    }

                                    logger->trace("response sent");
                                }
                            }
                            catch (const std::exception& ex)
                            {
                                logger->error("fatal exception in connection handler: {}", ex.what());
                            }

                            logger->trace("connection closing");
                        },
                        std::move(client_sock));
                }
            }
            catch (const std::exception& ex)
            {
                // TODO
                logger->critical("exception", ex.what());
            }
        }
    }

private:
    struct response_body : public io::writer<char>
    {
        explicit response_body(io::writer<char>* parent, server_response& resp)
            : parent(parent)
            , resp(resp)
        {}

        io::result write(const char* data, size_t length) override;

        io::writer<char>* parent;
        server_response&  resp;
        bool              is_write_started = false;
    };

    bool enforce_protocol(const request& req, server_response& resp) noexcept;

    net::listener                   listener;
    std::atomic_bool                is_serving;
    std::shared_ptr<spdlog::logger> logger;
    std::vector<std::thread>        connections;
    std::mutex                      connections_mu;

    size_t   max_header_bytes;
    uint16_t max_pending_connections;
};

}
