#pragma once

#include <chrono>
#include <expected>
#include <system_error>

#include "coro/task.hpp"
#include "http/request.hpp"
#include "http/response.hpp"
#include "io/scheduler.hpp"

namespace net::http
{

using namespace std::chrono_literals;

// client sends HTTP/1.x requests.
//
// Each request uses a new connection, which belongs to the response, and is closed along with it.
// TODO: reuse connections (which needs a connection pool that doesn't block, and knows when a response is finished).
class client
{
public:
    client(io::scheduler* scheduler, std::chrono::milliseconds connect_timeout = 10s, bool keepalives = true);

    client(const client&)            = default;
    client& operator=(const client&) = default;

    client(client&&) noexcept            = default;
    client& operator=(client&&) noexcept = default;

    ~client() = default;

    coro::task<std::expected<client_response, std::error_condition>> send(const client_request& request);

private:
    io::scheduler*            scheduler;
    std::chrono::milliseconds connect_timeout;
    bool                      keepalives;
};

}
