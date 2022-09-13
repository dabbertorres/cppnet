#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

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
    size_t               max_header_bytes;
    std::chrono::seconds header_read_timeout;
    uint16_t             max_pending_connections;
    // TODO: logging
    // TODO: threading
    // TODO: coroutines
};

class server
{
public:
    server(const std::string& port, const server_config& cfg);
    server(const std::string& host, const std::string& port, const server_config& cfg);

    server(const server&)            = delete;
    server& operator=(const server&) = delete;

    server(server&&) noexcept;
    server& operator=(server&&) noexcept;

    ~server() = default;

    void close();

    template<Handler H>
    void serve(H&& handler) const
    {
        listener.listen(max_pending_connections);

        while (is_serving)
        {
            // TODO: thread pool and coroutines

            try
            {
                auto                client_sock = listener.accept();
                io::buffered_reader reader{client_sock};
                io::buffered_writer writer{client_sock};

                // TODO: http/2

                auto req_result = http11::request_decode(reader);
                /* http2::request_decode(client_sock); */

                if (req_result.has_error())
                {
                    // TODO
                }

                auto req = req_result.to_value();

                server_response resp{
                    .version = req.version,
                    .body    = writer,
                };

                handler(req, resp);

                // TODO: this needs to start happening before the handler
                // e.g. large responses should be streamed, rather than
                // written to memory, and then written again.
                //
                // Options:
                //   * coroutine?
                //   * server_response interface only exposes methods for
                //     setting response code, headers, writing the body, etc
                if (auto err = http11::response_encode(writer, resp); err)
                {
                    // TODO
                }

                writer.flush();
            }
            catch (...)
            {
                // TODO
            }
        }
    }

private:
    net::listener    listener;
    std::atomic_bool is_serving;

    size_t   max_header_bytes;
    uint16_t max_pending_connections;
};
}
