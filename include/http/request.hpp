#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "coro/task.hpp"
#include "http/headers.hpp"
#include "http/http.hpp"
#include "http/request_target.hpp"
#include "io/buffered_reader.hpp"
#include "io/reader.hpp"
#include "io/string_reader.hpp"
#include "io/writer.hpp"
#include "tls/certificate.hpp"
#include "tls/version.hpp"
#include "url.hpp"
#include "util/small_vector.hpp"
#include "util/string_util.hpp"
#include "util/type_map.hpp"

namespace net::http
{

enum class request_method : std::uint8_t
{
    CONNECT,
    DELETE,
    GET,
    HEAD,
    OPTIONS,
    PATCH,
    POST,
    PUT,
    TRACE,
    NONE,
};

constexpr request_method parse_method(std::string_view str) noexcept
{
    using enum request_method;
    using namespace std::literals::string_view_literals;

    if (util::equal_ignore_case(str, "CONNECT"sv)) return CONNECT;
    if (util::equal_ignore_case(str, "DELETE"sv)) return DELETE;
    if (util::equal_ignore_case(str, "GET"sv)) return GET;
    if (util::equal_ignore_case(str, "HEAD"sv)) return HEAD;
    if (util::equal_ignore_case(str, "OPTIONS"sv)) return OPTIONS;
    if (util::equal_ignore_case(str, "PATCH"sv)) return PATCH;
    if (util::equal_ignore_case(str, "POST"sv)) return POST;
    if (util::equal_ignore_case(str, "PUT"sv)) return PUT;
    if (util::equal_ignore_case(str, "TRACE"sv)) return TRACE;

    [[unlikely]] return NONE;
}

constexpr std::string_view method_string(request_method m) noexcept
{
    using enum request_method;

    switch (m)
    {
    case CONNECT: return "CONNECT";
    case DELETE: return "DELETE";
    case GET: return "GET";
    case HEAD: return "HEAD";
    case OPTIONS: return "OPTIONS";
    case PATCH: return "PATCH";
    case POST: return "POST";
    case PUT: return "PUT";
    case TRACE: return "TRACE";
    default: return "NONE";
    }
}

// status is a response's status code (see http/response.hpp, which includes this header).
enum class status : std::uint16_t;

// client_request represents an outgoing HTTP request from a client to a server.
struct client_request
{
    request_method     method = request_method::GET;
    protocol_version   version{.major = 1, .minor = 1};
    url                uri{};
    net::http::headers headers{};

    // The body, which is read until it ends (or until Content-Length bytes have been read, if that's set, which it
    // must then match). Without a Content-Length, it's sent chunked, which needs HTTP/1.1. Sending the request reads
    // it, so it can only be sent once.
    std::unique_ptr<io::reader> body = nullptr;

    // get_body returns a new reader of the same body, from its start (Go's GetBody), for a redirect that sends it again
    // (307 and 308: see client::send()): without it, or if it returns null, a request with a body isn't redirected by
    // them (the 307 or 308 is returned). set_body() sets it. It's called on the task sending the request, so must be
    // quick (as any of the scheduler's tasks); an exception it throws comes out of send().
    std::function<std::unique_ptr<io::reader>()> get_body{};

    // stop cancels the request: requesting stop ends it wherever it is, failing with std::errc::operation_canceled,
    // whether it's waiting for a connection (or an HTTP/2 stream), connecting, sending the request, or waiting for the
    // response's head; and once send() has returned the response, reading its body fails the same way (unless it was
    // read to its end before). A canceled HTTP/1.x request's connection is closed, as it may be part way through the
    // request or the response; an HTTP/2 request's stream is reset (RST_STREAM with CANCEL), and its connection kept. A
    // canceled request isn't sent again. Resolving the host can't be interrupted, so stop requested meanwhile is seen
    // once it's done; nor can reading the request's body, which is seen once that read returns. A token that can't be
    // stopped (the default) costs nothing.
    //
    // E.g. a server's handler passes its request's token (server_request::stop) on, so its outgoing requests are
    // canceled with it: out.stop = req.stop.
    std::stop_token stop{};

    // max_redirects is how many redirects to follow for this request, overriding client_options::max_redirects (which
    // says what following them does) if it's set: e.g. 0 to get a redirect's response, from a client that follows
    // them.
    std::optional<std::size_t> max_redirects{};

    // on_informational is called for each interim (1xx) response that comes before the final one, over HTTP/1.1 and
    // HTTP/2 (e.g. 100 Continue, or 103 Early Hints, whose Link fields a caller may act on before the final response),
    // with its status and fields, which are only valid during the call; never for 101, which is final over HTTP/1.1
    // (and malformed over HTTP/2). It's called on the task sending the request, before send() returns, so must be quick
    // (as any of the scheduler's tasks); an exception it throws comes out of send(), the request abandoned (its
    // HTTP/1.x connection closed, or its HTTP/2 stream reset). It's kept for the requests a redirect makes, so it's
    // called for each one's interim responses. Without it, interim responses are skipped.
    //
    // A response's interim heads are limited altogether, whether or not this is set: over HTTP/1.x, with the final
    // head, to the client's limit on a response's head (1 MiB); over HTTP/2, to 1 MiB (and 32 bytes a field, as HTTP/2
    // counts them), past which the stream's reset. Either fails the request with std::errc::value_too_large.
    std::function<void(status, const net::http::headers&)> on_informational{};

    // make_trailers, if set, is called once the body has been read to its end, to fill the request's trailer fields
    // (e.g. a digest of the body, which its reader computed), which are sent after it: over HTTP/2, as a HEADERS frame
    // ending the stream (an empty DATA frame ends it instead if it leaves them empty), for any request, even one
    // without a body (it's called once the head's sent, and the head doesn't end the stream); over HTTP/1.1, in the
    // last chunk, so only for a body sent chunked (without a Content-Length), and it isn't called otherwise. Fields
    // that can't be trailers are dropped (see trailer_allowed()); an invalid name or value (e.g. a line break) fails
    // the request with std::errc::invalid_argument, cut short (its HTTP/1.1 connection closed, or its HTTP/2 stream
    // reset), so the server doesn't take it as complete.
    //
    // As get_body, it runs on the task sending the request, so must be quick; an exception it throws comes out of
    // send(), the request abandoned. It's called again each time the body is sent again (a redirect's 307 or 308, or
    // a retry). Nothing sends TE: trailers (which says the caller won't drop the response's trailers) unless the
    // caller sets it; over HTTP/1.1, TE is then listed in Connection (RFC 9110, section 10.1.4).
    std::function<void(net::http::trailer_fields&)> make_trailers{};

    // set_body sets the body to content, and the Content-Length to match; and get_body, to read it again (sharing
    // content, rather than copying it), for redirects that send it again.
    void set_body(std::string content)
    {
        headers.set_content_length(content.size());

        auto shared = std::make_shared<const std::string>(std::move(content));
        body        = std::make_unique<io::shared_string_reader>(shared);
        get_body    = [shared = std::move(shared)] { return std::make_unique<io::shared_string_reader>(shared); };
    }
};

// server_request represents an incoming HTTP request from a client to a server.
struct server_request
{
    request_method     method = request_method::NONE;
    protocol_version   version{};
    request_target     target{};
    net::http::headers headers{};

    // The host the request is for: from the target, if it has one, otherwise the Host header.
    std::string host{};

    // The TLS version the request's connection agreed on, if it came over TLS (HTTPS), or tls::version::none if it
    // didn't (always, in builds without TLS): e.g. tls_version != tls::version::none for building absolute URLs, or
    // Secure cookies.
    tls::version tls_version = tls::version::none;

    // The chain of certificates the client sent, leaf first, verified, if the request came over TLS from a server that
    // asks for clients' certificates (tls::server_options::client_auth), and the client sent one; null otherwise
    // (always, in builds without TLS). E.g. a handler (or middleware) authorizes the client by its leaf's subject, or a
    // SPIFFE ID among its uris, answering 403 for one it doesn't know. Shared by the connection's requests (and
    // HTTP/2's streams), so it's made once per connection, and a copy outlives the request.
    std::shared_ptr<const std::vector<tls::certificate>> peer_certificates{};

    // stop is stopped once the request is no longer wanted: over HTTP/2, when the client resets its stream, the stream
    // times out, or its connection ends; over HTTP/1.1, when its connection ends; and for either, when the server is
    // aborted (abort(), its destructor, or the scheduler's request_stop(); not close(), which lets requests finish).
    // Canceling is cooperative: nothing is ended for the handler, which learns of it by checking
    // stop.stop_requested() (e.g. between the steps of a long computation), by registering a std::stop_callback (which
    // must be quick, and not block: it runs on whichever thread stopped it, e.g. the connection's reader), or by
    // passing it to the waits that take one (e.g. scheduler::sleep_until(deadline, stop), socket::set_stop_token()),
    // which then fail with std::errc::operation_canceled. A handler stopped by the server's abort may still respond
    // (e.g. 503); otherwise its client is gone. A copy stays valid after the handler returns.
    //
    // The server sets it before calling the handler, so middleware may replace it for the handler it wraps (e.g. with
    // a token of its own, stopped by a deadline, or by this one through a std::stop_callback), putting it back after.
    std::stop_token stop{};

    // aborted is stopped once the server is aborted (abort(), its destructor, or the scheduler's request_stop()): so
    // middleware that finds stop stopped can tell the server's doing from the client's (e.g. http_metrics records 503
    // rather than 499). It isn't a reason: only whether the server aborted, which it may have done after the client's
    // side stopped the request. The server sets it for the connection (HTTP/1.1) or stream (HTTP/2): a copy of the
    // server's token, so nothing's allocated for it.
    std::stop_token aborted{};

    // The pattern of the route that matched the request (e.g. "/users/:id"), which the router sets; empty if none did.
    // It views the router's copy, so is only valid while the router is (as it is while the router handles it).
    std::string_view route{};

    // The route's parameters (name, then decoded value), which the router sets: see path_value. The values view the
    // request's path, or path_value_storage if they needed decoding, so are only valid while the request is, and
    // isn't moved (as it isn't while it's being handled).
    util::small_vector<std::pair<std::string_view, std::string_view>, 4> path_values{};

    // path_value_storage holds the path_values that needed decoding, for the router: it's reserved to the length of
    // the rest of the path from the first of them, which they can't be longer than together, so they never move.
    std::string path_value_storage{};

    // Values for handling the request, which middleware passes to handlers: e.g. instrument::trace.
    util::type_map extensions{};

    // The body, which ends (io::errc::end_of_stream) only once all of it has been read: one cut short fails (with
    // std::errc::connection_aborted, if the connection ends first).
    std::unique_ptr<io::reader> body = nullptr;

    // The request's trailer fields, once the body has been read to its end (until then, and for a body without any,
    // empty): over HTTP/1.1, a chunked body's trailer section; over HTTP/2, a HEADERS frame after the body. They're
    // kept apart from headers (RFC 9110, section 6.5), and without the fields that can't be trailers (see
    // trailer_allowed()), which are dropped. clear() empties them.
    net::http::trailer_fields trailers{};

    // path_value is the value of the route's parameter name: e.g. "42" for "id", for the route "/users/:id" and the
    // path "/users/42". It's empty if the route has no such parameter (a parameter never matches an empty segment).
    [[nodiscard]] std::string_view path_value(std::string_view name) const noexcept
    {
        const auto* iter = std::ranges::find(path_values, name, &std::pair<std::string_view, std::string_view>::first);
        return iter != path_values.end() ? iter->second : std::string_view{};
    }

    // add_path_value adds the parameter name, for encoded, which it decodes if need be. encoded must be part of
    // target.path, after any added before it, and name must outlive the request (e.g. being the router's).
    void add_path_value(std::string_view name, std::string_view encoded);

    // clear empties the request, to decode another into it, keeping its memory (and body, which decoding replaces), and
    // tls_version, peer_certificates, stop and aborted, as the next request comes over the same connection (whose
    // server sets stop for each request).
    void clear() noexcept
    {
        method  = request_method::NONE;
        version = {};
        target.path.clear();
        target.query.clear();
        target.host.clear();
        target.port.clear();
        headers.clear();
        trailers.clear();
        host.clear();
        route = {};
        path_values.clear();
        path_value_storage.clear();
        extensions.clear();
    }
};

namespace detail
{
    // expectation is what a request's Expect field asks of the server (RFC 9110, section 10.1.1).
    enum class expectation : std::uint8_t
    {
        none,         // nothing: no Expect field (or an empty one); or an HTTP/1.0 request, which has no expectations
        continue_100, // 100-continue: the client may wait for a 100 (Continue) before it sends the body
        unknown,      // anything else, which the server can't meet: it answers 417 (Expectation Failed)
    };

    // expectation_of is what req expects. Its Expect field is a list: each of its members must be 100-continue (in
    // any case, without parameters, which it has none of), or the expectation is unknown.
    [[nodiscard]] expectation expectation_of(const server_request& req);
}

using request_decoder_result = std::expected<server_request, std::error_code>;
using request_encoder_result = std::expected<io::writer*, std::error_code>;

// request_decoders borrow the connection's reader, which must outlive the decoded request's body. The reader is kept
// for the connection, rather than per request, so that anything buffered past the end of one request is still there
// for the next.
using request_decoder = coro::task<request_decoder_result> (*)(io::buffered_reader*, std::size_t) noexcept;
using request_encoder = coro::task<request_encoder_result> (*)(io::writer*, const client_request&) noexcept;

}
