#include "http/server.hpp"

#include <string>

#include "http/http.hpp"
#include "exception.hpp"

namespace net::http
{

server::server(std::string_view port, const server_config& cfg)
    : listener(port, network::tcp, addr_protocol::not_care, cfg.header_read_timeout),
      serve{false},
      max_header_bytes{cfg.max_header_bytes}
{}

server::server(std::string_view host, std::string_view port, const server_config& cfg)
    : listener(host, port, network::tcp, addr_protocol::not_care, cfg.header_read_timeout),
      serve{false},
      max_header_bytes{cfg.max_header_bytes}
{}

void server::close()
{
    serve = false;
}

void server::listen(handler& handler) const
{
    // TODO 32 was randomly picked
    if (!listener.listen(32))
        throw exception{}; // errno

    while(serve)
    {
        auto client_sock = listener.accept();

        request req{client_sock};
        parse_headers(client_sock, req);

        response resp{client_sock};
        handler.handle(req, resp);
        client_sock.flush();
    }
}

}
