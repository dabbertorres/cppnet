#pragma once

#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>

#include "coro/task.hpp"
#include "http/connection_pool.hpp"
#include "http/request.hpp"
#include "http/response.hpp"
#include "io/scheduler.hpp"

namespace net::http
{

// client sends HTTP/1.x requests, reusing connections within the limits of its client_options.
//
// A connection is reused once the response's body has been read to its end (until a read reports
// io::errc::end_of_stream, or all of its Content-Length has been read). Destroying a response whose body hasn't been
// read to its end closes its connection.
//
// Copies of a client share its connections. They're all closed once the client (and its copies) and the responses
// using them are gone, and none may outlive the scheduler.
class client
{
public:
    explicit client(io::scheduler* scheduler, const client_options& options = {});

    client(const client&)            = default;
    client& operator=(const client&) = default;

    client(client&&) noexcept            = default;
    client& operator=(client&&) noexcept = default;

    ~client() = default;

    // send sends a request, and returns the response once its head has been received. It takes the request (whose
    // body it reads), so a request lives as long as the task does, even if it's awaited later. Requests that are safe
    // to repeat (e.g. GET) are sent again (once) if a reused connection turns out to have been closed by the server.
    //
    // It fails with std::errc::timed_out if connecting takes longer than connect_timeout, the response's head longer
    // than response_header_timeout, or sending the request stalls for io_timeout. Reading the body fails the same way
    // if it stalls for io_timeout.
    coro::task<std::expected<client_response, std::error_code>> send(client_request request);

    // get sends a GET request for url (e.g. "http://example.com/path"), as send() does. It fails with
    // std::errc::invalid_argument if url isn't valid.
    coro::task<std::expected<client_response, std::error_code>> get(std::string_view url);

    // close_idle closes the connections that are waiting to be reused.
    void close_idle() noexcept;

    [[nodiscard]] connection_stats stats() const;
    [[nodiscard]] connection_stats stats(const std::string& host, const std::string& port) const;

private:
    std::shared_ptr<detail::connection_pool> pool;
};

}
