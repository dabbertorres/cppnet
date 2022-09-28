#include "http/server.hpp"

namespace net::http
{

server::server(const server_config& cfg)
    : listener(cfg.host,
               cfg.port,
               network::tcp,
               protocol::not_care,
               std::chrono::duration_cast<std::chrono::microseconds>(cfg.header_read_timeout))
    , is_serving{false}
    , logger{cfg.logger}
    , max_header_bytes{cfg.max_header_bytes}
    , max_pending_connections{cfg.max_pending_connections}
{}

server::server(server&& other) noexcept
    : listener{std::move(other.listener)}
    , is_serving{other.is_serving.exchange(false)}
    , logger{std::move(other.logger)}
    , max_header_bytes{other.max_header_bytes}
    , max_pending_connections{other.max_pending_connections}
{}

server& server::operator=(server&& other) noexcept
{
    if (is_serving)
    {
        close();
        listener.~listener();
    }

    is_serving              = other.is_serving.exchange(false);
    logger                  = std::move(other.logger);
    max_header_bytes        = other.max_header_bytes;
    max_pending_connections = other.max_pending_connections;

    return *this;
}

server::~server() { close(); }

void server::close()
{
    is_serving = false;
    is_serving.notify_all();
    logger->flush();
}

bool server::enforce_protocol(const request& req, server_response& resp) noexcept
{
    // TODO
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
