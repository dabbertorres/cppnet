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

// client sends HTTP/1.x and HTTP/2 requests, reusing connections within the limits of its client_options: to http://
// URLs, and (with NET_HAS_TLS) https:// ones, over TLS, verifying the server's certificate against
// client_options::tls's trusted certificate authorities (the system's, by default). Without NET_HAS_TLS, https fails
// with std::errc::protocol_not_supported.
//
// HTTP/2 is spoken to servers that agree on it (or, over cleartext, with client_options::http2_prior_knowledge): each
// request takes a stream of a connection the host's requests share. A request's version and Connection field are
// HTTP/1.x's, and mean nothing over HTTP/2 (its connections aren't closed after a request, and there's no HTTP/1.0):
// client_options::http2 = false is how to have them.
//
// An HTTP/1.x connection is reused once the response's body has been read to its end (until a read reports
// io::errc::end_of_stream, or all of its Content-Length has been read). Destroying a response whose body hasn't been
// read to its end closes its connection; over HTTP/2, it resets its stream.
//
// Copies of a client share its connections. They're all closed once the client (and its copies) and the responses
// using them are gone (HTTP/2 connections once their responses' bodies are done with, as those hold them), and none
// may outlive the scheduler.
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
    // to repeat (e.g. GET), and have no body, are sent again (once) if a reused HTTP/1.x connection turns out to have
    // been closed by the server, or an HTTP/2 connection drops (without GOAWAY) before the response's head. Over
    // HTTP/2, a request the server didn't process (it was refused, or after its GOAWAY's last stream) is sent again,
    // once, on another connection, whatever its method, if it has no body; and so is one refused before anything of it
    // was sent, even with a body. A body with a Content-Length is sent to that length, and must be at least as long
    // (it fails with std::errc::invalid_argument if it isn't).
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
