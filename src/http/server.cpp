#include "http/server.hpp"

#include <string>

#include "exception.hpp"
#include "http/http.hpp"

namespace net::http
{

server::server(std::string_view port, const server_config& cfg) :
    listener(port, network::tcp, protocol::not_care, cfg.header_read_timeout),
    serve{false},
    max_header_bytes{cfg.max_header_bytes},
    max_pending_connections{cfg.max_pending_connections}
{}

server::server(std::string_view host, std::string_view port, const server_config& cfg) :
    listener(host, port, network::tcp, protocol::not_care, cfg.header_read_timeout),
    serve{false},
    max_header_bytes{cfg.max_header_bytes},
    max_pending_connections{cfg.max_pending_connections}
{}

void server::close() { serve = false; }

void server::listen(handler& handler) const
{
    listener.listen(max_pending_connections);

    while (serve)
    {
        try
        {
            auto client_sock = listener.accept();

            /* request req{client_sock}; */
            /* parse_headers(client_sock, req); */

            /* response resp{client_sock}; */
            /* handler.handle(req, resp); */
            client_sock.flush();
        }
        catch (...)
        {
            // TODO
        }
    }
}

}
