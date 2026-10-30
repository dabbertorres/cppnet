#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>

#include "coro/task.hpp"
#include "http/connection_pool.hpp"
#include "http/request.hpp"
#include "http/response.hpp"
#include "io/scheduler.hpp"
#include "url.hpp"

namespace net::http
{

// client_errc are the client's own errors (in client_category()), for following redirects (see client::send()).
enum class client_errc : std::uint8_t
{
    // a request was redirected more than its limit allows (client_options::max_redirects, or the request's).
    too_many_redirects = 1,
    // a redirect's Location isn't a valid URI reference.
    bad_redirect = 2,
};

[[nodiscard]] const std::error_category& client_category() noexcept;

[[nodiscard]] std::error_code make_error_code(client_errc err) noexcept;

class client;

namespace detail
{

    // pool_of is a client's connection pool (its copies'), for what connects with the client's configuration apart
    // from its requests (e.g. a WebSocket, whose connection is its own: see connection_pool::connect_apart()).
    [[nodiscard]] std::shared_ptr<connection_pool> pool_of(const client& c) noexcept;

}

// client sends HTTP/1.x and HTTP/2 requests, reusing connections within the limits of its client_options: to http://
// URLs, and (with NET_HAS_TLS) https:// ones, over TLS, verifying the server's certificate against
// client_options::tls's trusted certificate authorities (the system's, by default). Without NET_HAS_TLS, https fails
// with std::errc::protocol_not_supported.
//
// HTTP/2 is spoken to servers that agree on it (or, over cleartext, with client_options::http2_prior_knowledge): each
// request takes a stream of a connection the host's requests share. A request's version and Connection field are
// HTTP/1.x's, and mean nothing over HTTP/2 (its connections aren't closed after a request, and there's no HTTP/1.0):
// client_options::http2 = false is how to have them. A request's Priority field (RFC 9218) orders its body among the
// connection's others, and client_response::set_priority() changes its response's priority in flight. With
// client_options::http2_coalescing, a request to an https origin may take a stream of another host's connection, whose
// server has said (with ORIGIN, RFC 8336) that it serves the origin, and whose certificate is valid for the host.
//
// An HTTP/1.x connection is reused once the response's body has been read to its end (until a read reports
// io::errc::end_of_stream, or all of its Content-Length has been read). Destroying a response whose body hasn't been
// read to its end closes its connection; over HTTP/2, it resets its stream.
//
// Redirects are followed only when asked for (client_options::max_redirects, or client_request::max_redirects): see
// send().
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
    //
    // It fails with std::errc::operation_canceled once request.stop is requested (see client_request::stop), whatever
    // else went wrong meanwhile: if stop is requested before it returns, it returns that, rather than the response.
    //
    // A response of 301, 302, 303, 307 or 308 with a Location is followed, up to the request's limit on redirects
    // (client_request::max_redirects, if it's set, otherwise client_options::max_redirects; 0, the default, follows
    // none): the next request, to Location resolved against the request's URL, is sent as any other (with its own
    // retries, connection and timeouts; request.stop covers them all), and send() returns the last response, whose url
    // says what it answers. One more redirect than the limit fails with client_errc::too_many_redirects; a Location
    // that isn't a valid URI reference with client_errc::bad_redirect, and one that isn't http or https with
    // std::errc::protocol_not_supported. 301 and 302 (for methods other than GET and HEAD) and 303 (other than HEAD)
    // are followed with a GET, and none of them sends the body, nor its fields (Content-Length, Content-Type,
    // Content-Encoding, Content-Language, Content-Location, Transfer-Encoding); 307 and 308 keep the method and the
    // body, which they read again from request.get_body (a request with a body, but no get_body, gets the 307 or 308
    // back). The fields go on to the next request, except Authorization, Proxy-Authorization and Cookie, which only go
    // to the same scheme, host and port, or the same host from http to https on the default ports (and once dropped,
    // to none of the later requests); and Host (if the request set it), which only goes to the same host and port.
    // There's no Referer. The URL's fragment is kept, unless Location has one. A redirect's response is read to its end
    // (and discarded) if what's left of its body is at most 2 KiB, so its HTTP/1.x connection may be reused; otherwise,
    // it's closed (over HTTP/2, its stream reset).
    //
    // A request with "Expect: 100-continue" and a body (not of length 0) sends its head alone (over HTTP/1.1; over
    // HTTP/2, without ending the stream), then waits for the server's first response, up to
    // client_options::expect_continue_timeout: a 100 (Continue), or none in time, sends the body; another interim
    // response is passed on (see client_request::on_informational), and the wait goes on, to the same deadline. A final
    // response first is returned. Over HTTP/1.1, the body's then sent only if the connection's to be reused (its
    // framing needs it), otherwise not, nor is the connection reused; over HTTP/2, it's not sent after a status above
    // 299 (the stream's reset once the response is done with), and is after a 2xx, once the wait's over (the server may
    // want both ways at once). A 417 (Expectation Failed) to such a request, whose body wasn't sent, has it sent again
    // at once, once, without its Expect field (over HTTP/1.1 on another connection, over HTTP/2 on the same one), its
    // body read then (so no get_body is needed); a 417 once the body's been sent is returned. response_header_timeout
    // starts once the body's sent.
    //
    // A request coalesced onto another host's HTTP/2 connection (see client_options::http2_coalescing) that gets a 421
    // (Misdirected Request) has its origin removed from that connection's Origin Set (RFC 8336), and is sent again, at
    // once, once, on a connection of its own host's, not coalesced; the 421 is discarded, as a redirect's response is.
    // One with a body needs request.get_body to read it again, as for a 307: without, the 421 is returned. So is a 421
    // on a connection of the request's own host's.
    coro::task<std::expected<client_response, std::error_code>> send(client_request request);

    // get sends a GET request for url (e.g. "http://example.com/path"), as send() does, canceled by stop (see
    // client_request::stop). It fails with std::errc::invalid_argument if url isn't valid.
    coro::task<std::expected<client_response, std::error_code>> get(std::string_view url, std::stop_token stop = {});

    // close_idle closes the connections that are waiting to be reused.
    void close_idle() noexcept;

    [[nodiscard]] connection_stats stats() const;
    [[nodiscard]] connection_stats stats(const std::string& host, const std::string& port) const;

private:
    friend std::shared_ptr<detail::connection_pool> detail::pool_of(const client& c) noexcept;

    std::shared_ptr<detail::connection_pool> pool;
};

namespace detail
{

    // keeps_credentials is whether a redirect from one URL to another keeps the request's credentials (Authorization,
    // Proxy-Authorization and Cookie): it's to the same scheme, host and port, or to the same host from http to https,
    // on the default ports. A URL without a scheme is http's, and one without a port is on its scheme's default.
    [[nodiscard]] bool keeps_credentials(const url& from, const url& to) noexcept;

}

}

namespace std
{

template<>
struct is_error_code_enum<net::http::client_errc> : std::true_type
{};

}
