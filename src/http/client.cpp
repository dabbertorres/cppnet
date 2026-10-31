#include "http/client.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <compare>
#include <cstddef>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "coro/task.hpp"
#include "http/connection.hpp"
#include "http/headers.hpp"
#include "http/http.hpp"
#include "http/http11.hpp"
#include "http/http2.hpp"
#include "http/http2_client.hpp"
#include "http/origin.hpp"
#include "http/request.hpp"
#include "http/response.hpp"
#include "io/buffered_reader.hpp"
#include "io/buffered_writer.hpp"
#include "io/io.hpp"
#include "io/reader.hpp"
#include "io/scheduler.hpp"
#include "socket.hpp"
#include "tcp.hpp"
#include "url.hpp"
#include "util/string_util.hpp"

namespace
{

using namespace std::string_view_literals;

// a server's response head can't use up all of the client's memory.
constexpr std::size_t max_response_header_bytes = std::size_t{1'024} * 1'024;

// pooled_body reads a response's body from a leased connection, which it returns to the pool once the body has been
// read to its end (or closes, if it's destroyed before then, or a read fails).
//
// Its request's stop token (set on the connection's socket, so a read waiting for it is canceled) fails the reads once
// stop is requested, unless the body was done with before: the connection is then closed, as what's left of the
// response is unknown. A failed read's error is returned again by the reads after it.
//
// It keeps a chunked body's trailers (see client_response::trailers()), which outlive the body's reader.
class pooled_body final
    : public net::io::reader
    , public net::http::detail::body_source
{
public:
    pooled_body(std::shared_ptr<net::http::detail::connection_pool> pool,
                net::http::detail::connection_pool::lease           leased,
                std::stop_token                                     stop)
        : pool{std::move(pool)}
        , leased{std::move(leased)}
        , reader{std::make_unique<net::io::buffered_reader>(&net::http::detail::as_reader(*this->leased.conn),
                                                            this->pool->options().read_buffer_size)}
        , stop{std::move(stop)}
    {}

    pooled_body(const pooled_body&)            = delete;
    pooled_body& operator=(const pooled_body&) = delete;
    pooled_body(pooled_body&&)                 = delete;
    pooled_body& operator=(pooled_body&&)      = delete;

    ~pooled_body() override { finish(false); }

    [[nodiscard]] net::http::detail::connection*   connection() const noexcept { return leased.conn.get(); }
    [[nodiscard]] net::io::buffered_reader*        buffered() const noexcept { return reader.get(); }
    [[nodiscard]] const net::http::client_options& options() const noexcept { return pool->options(); }

    // attach sets the body that's read from the connection, resp's (keeping its trailers, if it's chunked). length is
    // how long it is, if it's known up front.
    void attach(net::http::client_response& resp, bool reuse, std::optional<std::size_t> length)
    {
        net::http::http11::keep_body_trailers(resp, kept);

        body      = std::move(resp.body);
        reusable  = reuse;
        remaining = length;

        if (remaining == 0) finish(true);
    }

    net::coro::task<net::io::result> read(std::span<std::byte> data) override
    {
        if (body == nullptr) co_return {.err = failed ? failed : make_error_code(net::io::errc::end_of_stream)};

        // NOTE: checked first, as what's buffered would be read without waiting (so without the socket's token).
        if (stop.stop_requested())
        {
            failed = std::make_error_code(std::errc::operation_canceled);
            finish(false);
            co_return {.err = failed};
        }

        auto res = co_await body->read(data);

        if (res.err)
        {
            const bool complete = res.err == net::io::errc::end_of_stream;

            // whatever the cancellation caused (e.g. a TLS read cut short), it's reported as what it was.
            if (!complete && stop.stop_requested()) res.err = std::make_error_code(std::errc::operation_canceled);
            if (!complete) failed = res.err;
            finish(complete);
        }
        else if (remaining.has_value())
        {
            // done as soon as the last byte has been read, rather than at the next read (which may not come).
            *remaining -= std::min(*remaining, res.count);
            if (*remaining == 0) finish(true);
        }

        co_return res;
    }

    using net::io::reader::read;

    [[nodiscard]] net::io::handle native_handle() const noexcept override
    {
        return leased.conn != nullptr ? net::http::detail::native_handle(*leased.conn) : -1;
    }

    [[nodiscard]] const net::http::trailer_fields& trailers() const noexcept override { return kept; }

    // (HTTP/1.1 has no way to change a response's priority once it's been asked for.)
    bool set_priority(const net::http::priority& /*p*/) override { return false; }

private:
    // finish returns the connection to the pool, which may reuse it if the whole body was read.
    void finish(bool complete) noexcept
    {
        if (leased.conn == nullptr) return;

        // nothing is pipelined, so anything after the body is the server misbehaving.
        const bool reuse = complete && reusable && reader->buffered().empty();

        // an idle connection holds no request's token.
        net::http::detail::socket_of(*leased.conn).set_stop_token({});

        body   = nullptr;
        reader = nullptr;
        pool->release(std::move(leased), reuse);
    }

    std::shared_ptr<net::http::detail::connection_pool> pool;
    net::http::detail::connection_pool::lease           leased;
    std::unique_ptr<net::io::buffered_reader>           reader;
    std::unique_ptr<net::io::reader>                    body;
    bool                                                reusable = false;
    std::optional<std::size_t>                          remaining;
    std::stop_token                                     stop;
    std::error_code                                     failed; // a read failed (see read())
    net::http::trailer_fields                           kept;   // the body's trailers, once it's read to its end
};

// limit is a timeout from client_options, where 0 is no limit.
std::optional<net::io::clock::duration> limit(std::chrono::milliseconds timeout) noexcept
{
    if (timeout <= std::chrono::milliseconds::zero()) return std::nullopt;
    return timeout;
}

bool is_idempotent(net::http::request_method method) noexcept
{
    using enum net::http::request_method;
    return method == GET || method == HEAD || method == OPTIONS || method == TRACE || method == PUT || method == DELETE;
}

// endpoint is the scheme ("http", unless it's "https") and port (the scheme's default, if the URL has none) that a
// request is sent to. It fails with std::errc::protocol_not_supported for other schemes. (The pool refuses https
// without TLS.)
std::expected<std::pair<std::string, std::string>, std::error_code> endpoint(const net::url& uri)
{
    const bool https = net::util::equal_ignore_case(std::string_view{uri.scheme}, "https"sv);
    if (!https && !uri.scheme.empty() && !net::util::equal_ignore_case(std::string_view{uri.scheme}, "http"sv))
        return std::unexpected(std::make_error_code(std::errc::protocol_not_supported));

    return std::pair{std::string{https ? "https" : "http"},
                     !uri.port.empty() ? uri.port : std::string{https ? "443" : "80"}};
}

// closed_early is whether a request failed as it would on a connection the server had closed before getting it.
bool closed_early(std::error_code err) noexcept { return err == net::io::status_condition::closed; }

bool has_close(const net::http::headers& fields)
{
    return std::ranges::any_of(fields.get_list("Connection"sv),
                               [](auto option) { return net::util::equal_ignore_case(option, "close"sv); });
}

// known_length is the length of the response's body, if it's known before reading it.
std::optional<std::size_t> known_length(const net::http::client_response& resp, net::http::request_method method)
{
    using net::http::status;

    if (method == net::http::request_method::HEAD || resp.status_code == status::NO_CONTENT
        || resp.status_code == status::NOT_MODIFIED)
        return 0;

    if (resp.headers.get_all("Transfer-Encoding"sv).has_value()) return std::nullopt;
    return resp.headers.get_content_length();
}

// the most of a redirect's response's body that's read (and discarded), so its connection may be reused: Go's.
constexpr std::size_t max_discarded = std::size_t{2} * 1'024;

bool is_redirect(net::http::status code) noexcept
{
    using enum net::http::status;
    return code == MOVED_PERMANENTLY || code == FOUND || code == SEE_OTHER || code == TEMPORARY_REDIRECT
        || code == PERMANENT_REDIRECT;
}

// scheme_of is the scheme a URL's requests are sent with: http, unless it's https (whatever its case).
std::string_view scheme_of(const net::url& uri) noexcept
{
    return net::util::equal_ignore_case(std::string_view{uri.scheme}, "https"sv) ? "https"sv : "http"sv;
}

// port_of is the port a URL's requests are sent to: its own, or its scheme's default.
std::string_view port_of(const net::url& uri) noexcept
{
    if (!uri.port.empty()) return uri.port;
    return scheme_of(uri) == "https"sv ? "443"sv : "80"sv;
}

// same_server is whether two URLs' requests go to the same host and port.
bool same_server(const net::url& from, const net::url& to) noexcept
{
    return net::util::equal_ignore_case(std::string_view{from.host}, std::string_view{to.host})
        && port_of(from) == port_of(to);
}

// redirect turns request into the one that follows its response, if the response is a redirect to follow (see
// client::send()), returning whether it did: followed counts the redirects followed so far, and limit is the most
// that may be. A redirect that can't be followed fails, leaving request as it was.
std::expected<bool, std::error_code> redirect(net::http::client_request&        request,
                                              const net::http::client_response& resp,
                                              std::size_t                       limit,
                                              std::size_t&                      followed)
{
    using net::http::client_errc;
    using net::http::request_method;
    using net::http::status;

    if (!is_redirect(resp.status_code) || limit == 0) return false;

    const auto location = resp.headers.get("Location"sv);
    if (!location.has_value()) return false;

    // 307 and 308 send the body again, which needs get_body; 301, 302 and 303 never send it.
    const bool keep_body =
        resp.status_code == status::TEMPORARY_REDIRECT || resp.status_code == status::PERMANENT_REDIRECT;
    const bool resend_body = keep_body && request.body != nullptr;
    if (resend_body && !request.get_body) return false;

    if (followed >= limit) return std::unexpected(make_error_code(client_errc::too_many_redirects));

    auto next = net::url::resolve(request.uri, *location);
    if (!next.has_value())
    {
        if (next.error() == std::errc::invalid_argument)
            return std::unexpected(make_error_code(client_errc::bad_redirect));
        return std::unexpected(next.error());
    }

    if (auto target = endpoint(*next); !target.has_value()) return std::unexpected(target.error());

    // RFC 9110, section 10.2.2: a Location without a fragment keeps the request's.
    if (next->fragment.empty()) next->fragment = request.uri.fragment;

    std::unique_ptr<net::io::reader> body;
    if (resend_body)
    {
        body = request.get_body();
        if (body == nullptr) return false;
    }

    ++followed;

    // never to another server, nor from https to http; and once dropped, they're gone for the later redirects too.
    if (!net::http::detail::keeps_credentials(request.uri, *next))
    {
        request.headers.remove("Authorization"sv);
        request.headers.remove("Proxy-Authorization"sv);
        request.headers.remove("Cookie"sv);
    }

    // a Host the caller set is for the server it named.
    if (!same_server(request.uri, *next)) request.headers.remove("Host"sv);

    if (keep_body)
    {
        request.body = std::move(body);
    }
    else
    {
        // RFC 9110, section 15.4: as browsers do, a method other than GET or HEAD becomes GET; and the content goes,
        // with the fields that describe it.
        if (request.method != request_method::GET && request.method != request_method::HEAD)
            request.method = request_method::GET;

        // NOTE: its trailers go with it, as they describe it.
        request.body          = nullptr;
        request.get_body      = nullptr;
        request.make_trailers = nullptr;
        for (const auto field : {"Content-Length"sv,
                                 "Content-Type"sv,
                                 "Content-Encoding"sv,
                                 "Content-Language"sv,
                                 "Content-Location"sv,
                                 "Transfer-Encoding"sv})
            request.headers.remove(field);
    }

    request.uri = std::move(*next);
    return true;
}

// follow is redirect(), which also says whether the request still goes over HTTP/1.1, if it did (http1_only: see
// client::send()): only to the same scheme, host and port.
std::expected<bool, std::error_code> follow(net::http::client_request&        request,
                                            const net::http::client_response& resp,
                                            std::size_t                       limit,
                                            std::size_t&                      followed,
                                            bool&                             http1_only)
{
    // NOTE: copied only if it's needed, to compare with where the redirect goes.
    std::optional<net::url> from;
    if (http1_only) from = request.uri;

    auto followed_now = redirect(request, resp, limit, followed);
    if (from.has_value() && followed_now.value_or(false))
        http1_only = scheme_of(*from) == scheme_of(request.uri) && same_server(*from, request.uri);
    return followed_now;
}

// discard is done with a redirect's response: its body is read to its end if what's left of it is at most
// max_discarded, so its HTTP/1.x connection may be reused (and its HTTP/2 stream needn't be reset); otherwise it's
// closed (or reset) at once, as its response is destroyed before its end.
net::coro::task<> discard(net::http::client_response resp)
{
    if (resp.body == nullptr) co_return;

    // NOTE: a response that has no body (e.g. to HEAD) has been done with already, whatever its Content-Length says.
    if (const auto length = resp.headers.get_content_length(); length.has_value() && *length > max_discarded) co_return;

    // up to a byte more than max_discarded: a body no longer than that has ended by then.
    std::array<std::byte, 512> buf{};
    for (std::size_t read = 0; read <= max_discarded;)
    {
        const auto res = co_await resp.body->read(std::span{buf}.first(std::min(buf.size(), max_discarded + 1 - read)));
        if (res.err) co_return;
        read += res.count;
    }
}

// sent is what sending a request once made of it: its response (or why there's none), and whether to send it again.
struct sent
{
    std::expected<net::http::client_response, std::error_code> response;
    bool                                                       retry = false;

    // the retry may go on the same HTTP/2 connection (a new stream): after a 417, which says nothing of the connection.
    bool same_connection = false;

    // the retry's after a 421 (Misdirected Request) on another host's connection the request was coalesced onto: it
    // goes on a connection of its own host's, as if it were the first attempt.
    bool misdirected = false;

    // the retry's because the server asked for HTTP/1.1 (HTTP_1_1_REQUIRED): it goes on an HTTP/1.x connection to the
    // request's own host, as if it were the first attempt, and so do the attempts after it.
    bool http11 = false;
};

// attempt is how an attempt to send a request goes: whether it's a first (it may take an idle connection, and be sent
// again if it fails as it would on a connection the server had closed: see send_on()); whether it may be coalesced
// onto another host's HTTP/2 connection (client_options::http2_coalescing: not after a 421 on one, nor over
// HTTP/1.1); and whether it goes over HTTP/1.1 (the server asked for that).
struct attempt
{
    bool first      = true;
    bool coalesce   = true;
    bool http1_only = false;

    // after is how the attempt after one that came to result goes: a coalesced request that had a 421 goes again as
    // it went first, but to its own host; so does one the server wants over HTTP/1.1, from then on.
    void after(const sent& result) noexcept
    {
        first      = result.misdirected || result.http11;
        http1_only = http1_only || result.http11;
        coalesce   = coalesce && !result.misdirected && !http1_only;
    }
};

// head_deadline is when a response's head must have come by, from now.
std::optional<net::io::clock::time_point> head_deadline(const net::http::client_options& options)
{
    if (auto wait = limit(options.response_header_timeout)) return net::io::clock::now() + *wait;
    return std::nullopt;
}

// await_continue waits for what decides whether a request's body follows its head, which has been sent alone (see
// client_options::expect_continue_timeout): a 100 (Continue), or nothing within the timeout, says it does (returning
// nothing); a final response first is returned. Another interim response is passed on (with the 100), and the wait goes
// on, to the same deadline. Each head, once its first byte's in, has response_header_timeout to come whole.
net::coro::task<std::expected<std::optional<net::http::client_response>, std::error_code>>
await_continue(net::io::buffered_reader*            reader,
               net::socket&                         sock,
               const net::http::client_request&     request,
               const net::http::client_options&     options,
               net::http::http11::informational_ref on_informational)
{
    namespace io     = net::io;
    namespace http11 = net::http::http11;
    using net::http::status;

    const auto deadline = io::clock::now() + options.expect_continue_timeout;
    while (true)
    {
        // NOTE: checked with heads buffered too, so a server sending interim responses without end doesn't put it off.
        if (io::clock::now() >= deadline) co_return std::nullopt;

        if (reader->buffered().empty())
        {
            sock.set_read_deadline(deadline);
            if (auto err = co_await reader->fill(); err)
            {
                if (err != io::status_condition::timed_out) co_return std::unexpected(err);

                // nothing came (a timed-out read reads nothing): the reads after it start afresh.
                reader->reset(nullptr);
                co_return std::nullopt;
            }
        }

        sock.set_read_deadline(head_deadline(options));
        auto resp =
            co_await http11::response_decode(reader, request.method, max_response_header_bytes, on_informational, true);
        if (!resp.has_value()) co_return std::unexpected(resp.error());

        if (resp->status_code == status::CONTINUE) co_return std::nullopt;
        if (!net::http::status_is_information(resp->status_code) || resp->status_code == status::SWITCHING_PROTOCOLS)
            co_return std::optional{std::move(*resp)};
    }
}

// send_body sends the body of a request whose head was sent alone, read body_buffer at a time.
net::coro::task<std::error_code>
send_body(net::io::buffered_writer& out, const net::http::client_request& request, std::size_t body_buffer)
{
    auto res = co_await net::http::http11::request_encode_body(&out, request, body_buffer);
    if (!res.has_value()) co_return res.error();
    co_return (co_await out.flush()).err;
}

// deliver gives a response the body that reads it from the connection, which is reused once it's been read if reuse
// says it may be (the server got all of a request that doesn't close the connection), and the response allows it.
sent deliver(std::unique_ptr<pooled_body>&    body,
             net::http::client_response&      resp,
             const net::http::client_request& request,
             bool                             reuse)
{
    reuse = reuse && net::http::http11::connection_reusable(resp, request.method);
    body->attach(resp, reuse, known_length(resp, request.method));
    resp.body = std::move(body);
    return sent{.response = std::move(resp)};
}

// send_expecting sends the body of a request that expects 100 (Continue), whose head has been sent alone, once the wait
// for it's over (see await_continue()); or doesn't, if a final response came first: then it's what the request came
// to, as it is if the wait fails. Otherwise, the body's been sent (or failed to be, as written then says, though the
// response is still worth reading), and the response is to be read, with no read deadline left.
net::coro::task<std::optional<sent>> send_expecting(net::io::buffered_writer&            out,
                                                    net::socket&                         sock,
                                                    std::unique_ptr<pooled_body>&        body,
                                                    net::http::client_request&           request,
                                                    bool                                 reusable_request,
                                                    net::http::http11::informational_ref on_informational,
                                                    net::http::request_encoder_result&   written)
{
    const auto& options = body->options();

    auto waited = co_await await_continue(body->buffered(), sock, request, options, on_informational);
    if (!waited.has_value()) co_return sent{.response = std::unexpected(waited.error())};

    // a 100, or none in time: the body goes. A server may respond before reading all of it (e.g. 413 Content Too
    // Large), and then close the connection, failing the rest of the write: the response is still worth reading.
    if (!waited->has_value())
    {
        sock.set_read_deadline(std::nullopt);

        const auto err = co_await send_body(out, request, options.write_buffer_size);
        if (err && !closed_early(err)) co_return sent{.response = std::unexpected(err)};

        if (err) written = std::unexpected(err);
        co_return std::nullopt;
    }

    // a final response first. Its body's reads are only limited by io_timeout.
    auto& resp = **waited;
    sock.set_read_deadline(std::nullopt);

    // RFC 9110, section 10.1.1: something on the way doesn't do expectations, so the request's sent again without
    // (once: it's gone), at once, and on another connection, as this one's body never came. Its body's untouched, so it
    // needs no get_body.
    if (resp.status_code == net::http::status::EXPECTATION_FAILED)
    {
        request.headers.remove("Expect"sv);

        // NOTE: never returned, as the request's sent again (or that fails, with its own error).
        co_return sent{.response = std::unexpected(std::make_error_code(std::errc::protocol_error)), .retry = true};
    }

    // Go's rule: the body's sent if the connection's to be reused, whose framing needs it, and not if it isn't (the
    // connection then isn't).
    const bool kept =
        reusable_request && options.keep_alive && net::http::http11::connection_reusable(resp, request.method);
    if (!kept) co_return deliver(body, resp, request, false);

    const auto err = co_await send_body(out, request, options.write_buffer_size);
    co_return deliver(body, resp, request, !err);
}

// rewind_body gets a request's body to send again (see client_request::get_body), returning whether there is one to
// send: there's nothing to get for a request without a body.
bool rewind_body(net::http::client_request& request)
{
    if (request.body == nullptr) return true;
    if (!request.get_body) return false;

    auto body = request.get_body();
    if (body == nullptr) return false;

    request.body = std::move(body);
    return true;
}

// send_http2 sends a request on a stream of an HTTP/2 connection. It may be sent again (on another connection):
// - whatever it is, if it was refused before anything of it was sent (the connection was ending);
// - whatever its method, if the server didn't process it (it was refused, or after a GOAWAY's last stream), unless its
//   body's been read;
// - if it's safe to repeat, and has no body, if the connection dropped (without GOAWAY) before its response's head: as
//   HTTP/1.1's are, when a reused connection turns out to have been closed;
// - without its expectation, at once, if it expected 100 (Continue) and had a 417 before its body was sent: on the same
//   connection, as a 417 says nothing of it;
// - on a connection of its own host's, if it was coalesced onto another host's and had a 421 (Misdirected Request), and
//   has no body, or get_body to read it again (that 421 is the pool's doing, so the pool undoes it; one on the
//   request's own host's connection is returned, as another connection there would likely reach the same server); the
//   421 is discarded (as a redirect's is);
// - over HTTP/1.1, to its own host, if the server asked for that (HTTP_1_1_REQUIRED) before any response, whether
//   or not this was a first attempt, and it has no body, or get_body to read it again: if it's safe to repeat, or the
//   server didn't process it (it was after the last stream of the server's GOAWAY, or never sent). Otherwise, that's
//   returned.
net::coro::task<sent>
send_http2(net::http::detail::connection_pool::lease leased, net::http::client_request& request, bool first)
{
    net::http::detail::http2_client_connection::send_state state;

    auto resp = co_await leased.http2->send(request, &state);

    // RFC 9110, section 10.1.1: as over HTTP/1.1 (see send_expecting()), its body untouched. The 417's dropped (its
    // stream reset, as the request never ended), and the retry's on a new stream.
    if (state.expectation_failed)
    {
        request.headers.remove("Expect"sv);

        // NOTE: never returned, as the request's sent again (or that fails, with its own error).
        co_return sent{.response        = std::unexpected(std::make_error_code(std::errc::protocol_error)),
                       .retry           = true,
                       .same_connection = true};
    }

    // RFC 8336, section 2.3: the connection isn't authoritative for the request's origin after all, so its Origin Set
    // loses it, and the request goes where it would have gone without coalescing. (A 421 on the request's own host's
    // connection is returned: another to the same host would likely reach the same server.)
    if (leased.coalesced && resp.has_value() && resp->status_code == net::http::status::MISDIRECTED_REQUEST)
    {
        leased.http2->remove_origin(net::http::detail::origin_text(request.uri.host, port_of(request.uri)));
        if (!rewind_body(request)) co_return sent{.response = std::move(resp)};

        co_await discard(std::move(*resp));

        // NOTE: never returned, as the request's sent again (or that fails, with its own error).
        co_return sent{.response    = std::unexpected(std::make_error_code(std::errc::protocol_error)),
                       .retry       = true,
                       .misdirected = true};
    }

    // RFC 9113, section 7: the server wants the request over HTTP/1.1 (e.g. for a client certificate of its own, which
    // HTTP/2 can't ask for: section 9.2.1). Once it's answered anything, it's had the request, as for REFUSED_STREAM.
    if (!resp.has_value() && resp.error() == net::http::http2::error_code::http_1_1_required)
    {
        const bool again =
            state.unsent || state.unprocessed || (state.http11_required && is_idempotent(request.method));
        if (!again || !rewind_body(request)) co_return sent{.response = std::move(resp)};

        // NOTE: never returned, as the request's sent again (or that fails, with its own error).
        co_return sent{.response = std::unexpected(std::make_error_code(std::errc::protocol_error)),
                       .retry    = true,
                       .http11   = true};
    }

    if (resp.has_value() || !first) co_return sent{.response = std::move(resp)};

    const bool refused = resp.error() == net::http::http2::error_code::refused_stream;
    const bool retry =
        state.unsent || (request.body == nullptr && (refused || (state.dropped && is_idempotent(request.method))));
    co_return sent{.response = std::move(resp), .retry = retry};
}

// send_http11 sends a request on an HTTP/1.x connection, which the response's body gives back to the pool once it's
// been read. A request that's safe to repeat may be sent again if a reused connection turns out to have been closed.
net::coro::task<sent> send_http11(std::shared_ptr<net::http::detail::connection_pool> pool,
                                  net::http::detail::connection_pool::lease           leased,
                                  net::http::client_request&                          request,
                                  bool                                                first,
                                  bool                                                reusable_request)
{
    namespace io     = net::io;
    namespace http11 = net::http::http11;
    namespace detail = net::http::detail;

    // canceled before anything of it was sent: the connection is as it was, so it may be used for another.
    if (request.stop.stop_requested())
    {
        pool->release(std::move(leased), true);
        co_return sent{.response = std::unexpected(std::make_error_code(std::errc::operation_canceled))};
    }

    const bool reused = leased.reused;
    auto       body   = std::make_unique<pooled_body>(pool, std::move(leased), request.stop);
    auto*      conn   = body->connection();

    // NOTE: set every time, as a reused connection has whatever its last request left. The token cancels the reads and
    //       writes that wait (a canceled request's connection is closed: the body's destructor, or its failed read).
    const auto& options = pool->options();
    auto&       sock    = detail::socket_of(*conn);
    sock.set_deadline(std::nullopt);
    sock.set_timeout(limit(options.io_timeout));
    sock.set_stop_token(request.stop);

    // NOTE: not with a body, which has been read.
    const auto retry = [&](std::error_code err)
    { return reused && first && request.body == nullptr && is_idempotent(request.method) && closed_early(err); };

    // a request that expects 100 (Continue) sends its head alone, and its body once it's had the 100, or waited for it.
    // (an HTTP/1.0 server ignores the expectation.)
    const bool expecting =
        request.version.minor >= 1 && detail::expects_continue(request, options.expect_continue_timeout);

    // the head in one write, rather than one per field.
    io::buffered_writer out{&detail::as_writer(*body->connection()), 4'096};

    auto res = expecting ? co_await http11::request_encode_head(&out, request)
                         : co_await http11::request_encode(&out, request, options.write_buffer_size);
    if (res.has_value())
    {
        if (auto flushed = co_await out.flush(); flushed.err) res = std::unexpected(flushed.err);
    }

    // a server may respond before reading all of the body (e.g. 413 Content Too Large), and then close the
    // connection, failing the rest of the write. The response is still worth reading, if it's there.
    bool write_failed = !res.has_value();
    if (write_failed)
    {
        if (retry(res.error())) co_return sent{.response = std::unexpected(res.error()), .retry = true};
        if (request.body == nullptr || !closed_early(res.error()))
            co_return sent{.response = std::unexpected(res.error())};
    }

    // an interim response says the server has the request, so it's not sent again if the connection then ends (as over
    // HTTP/2, which counts the stream as processed); the caller's told of each (an exception it throws closes the
    // connection, as the body's destroyed with this frame).
    bool       interim          = false;
    const auto on_informational = [&request, &interim](net::http::status code, const net::http::headers& fields)
    {
        interim = true;
        if (request.on_informational) request.on_informational(code, fields);
    };

    // the body goes once the server's said to send it, or not soon enough, unless a final response came first.
    if (expecting && !write_failed)
    {
        if (auto done = co_await send_expecting(out, sock, body, request, reusable_request, on_informational, res))
            co_return std::move(*done);
        write_failed = !res.has_value();
    }

    // NOTE: a deadline, for the final head: interim responses don't put it off.
    if (auto wait = limit(options.response_header_timeout)) sock.set_read_deadline(io::clock::now() + *wait);

    auto resp =
        co_await http11::response_decode(body->buffered(), request.method, max_response_header_bytes, on_informational);

    // the body's reads are only limited by io_timeout.
    sock.set_read_deadline(std::nullopt);
    if (!resp.has_value())
    {
        if (write_failed) co_return sent{.response = std::unexpected(res.error())};
        co_return sent{.response = std::unexpected(resp.error()), .retry = !interim && retry(resp.error())};
    }

    // the server didn't get all of the request, so the connection is no use for another.
    co_return deliver(body, *resp, request, !write_failed && reusable_request);
}

// send_on sends a request on what was leased for it: a stream of an HTTP/2 connection, or an HTTP/1.x connection.
// NOTE: not a coroutine, so it adds no frame of its own.
net::coro::task<sent> send_on(std::shared_ptr<net::http::detail::connection_pool> pool,
                              net::http::detail::connection_pool::lease           leased,
                              net::http::client_request&                          request,
                              bool                                                first,
                              bool                                                reusable_request)
{
    if (leased.http2 != nullptr) return send_http2(std::move(leased), request, first);
    return send_http11(std::move(pool), std::move(leased), request, first, reusable_request);
}

// destination is where a request is sent: the scheme and port (see endpoint()), and whether its connection may be
// reused after it.
struct destination
{
    std::string scheme;
    std::string port;
    bool        reusable = false;
};

// destination_of checks that a request can be sent (before taking a connection, which one that can't would waste),
// and says where it's sent.
std::expected<destination, std::error_code> destination_of(const net::http::client_request& request)
{
    auto target = endpoint(request.uri);
    if (!target.has_value()) return std::unexpected(target.error());

    if (auto err = net::http::http11::check_request(request); err) return std::unexpected(err);

    // a request that closes its connection can't be followed by another (over HTTP/1.x: HTTP/2 has no such thing).
    return destination{.scheme   = std::move(target->first),
                       .port     = std::move(target->second),
                       .reusable = request.version.minor >= 1 && !has_close(request.headers)};
}

class client_category_impl final : public std::error_category
{
public:
    [[nodiscard]] const char* name() const noexcept override { return "net::http::client"; }

    [[nodiscard]] std::string message(int ev) const override
    {
        using net::http::client_errc;

        switch (static_cast<client_errc>(ev))
        {
        case client_errc::too_many_redirects: return "too many redirects";
        case client_errc::bad_redirect: return "a redirect's Location isn't a valid URI reference";
        default: return "(unrecognized)";
        }
    }
};

}

namespace net::http
{

const std::error_category& client_category() noexcept
{
    static const client_category_impl category;
    return category;
}

std::error_code make_error_code(client_errc err) noexcept { return {static_cast<int>(err), client_category()}; }

namespace detail
{

    bool keeps_credentials(const url& from, const url& to) noexcept
    {
        if (!util::equal_ignore_case(std::string_view{from.host}, std::string_view{to.host})) return false;

        const auto from_scheme = scheme_of(from);
        const auto to_scheme   = scheme_of(to);
        if (from_scheme == to_scheme) return port_of(from) == port_of(to);

        // an upgrade to https, on the default ports.
        return from_scheme == "http"sv && to_scheme == "https"sv && port_of(from) == "80"sv && port_of(to) == "443"sv;
    }

    std::shared_ptr<connection_pool> pool_of(const client& c) noexcept { return c.pool; }

}

client::client(io::scheduler* scheduler, const client_options& options)
    : pool{std::make_shared<detail::connection_pool>(scheduler, options)}
{}

coro::task<std::expected<client_response, std::error_code>> client::send(client_request request)
{
    // a request's version is the HTTP/1.x it's sent as: HTTP/2 is used when a connection agrees on it.
    if (request.version.major > 1) co_return std::unexpected(std::make_error_code(std::errc::not_supported));

    // a canceled request fails with that, whatever its cancellation caused (e.g. a connection shut part way through a
    // TLS record), or happened meanwhile; and it isn't sent again.
    const auto canceled = [&request] { return request.stop.stop_requested(); };
    const auto cancel   = [] { return std::unexpected(std::make_error_code(std::errc::operation_canceled)); };
    const auto fail     = [&](std::error_code err) { return canceled() ? cancel() : std::unexpected(err); };

    const std::size_t limit    = request.max_redirects.value_or(pool->options().max_redirects);
    std::size_t       followed = 0;

    // whether the server asked for HTTP/1.1 (see send_on()): for the rest of the hop, and the hops after it to the
    // same scheme, host and port (another would likely ask again); not for other requests.
    bool http1_only = false;

    // each redirect is a request of its own: its connection, retries and timeouts.
    while (true)
    {
        const auto dest = destination_of(request);
        if (!dest.has_value()) co_return std::unexpected(dest.error());

        // the HTTP/2 connection the last attempt was on: a retry is on another (but after a 417).
        std::shared_ptr<detail::http2_client_connection> tried;
        sent                                             result;

        attempt how{.first = true, .coalesce = !http1_only, .http1_only = http1_only};
        do
        {
            // a retry is because a reused connection had been closed, so the others may have been too.
            auto leased = co_await pool->acquire(dest->scheme,
                                                 request.uri.host,
                                                 dest->port,
                                                 how.first, // (allow_idle)
                                                 tried.get(),
                                                 request.stop,
                                                 how.coalesce,
                                                 how.http1_only);
            if (!leased.has_value()) co_return fail(leased.error());

            // HTTP/2: the request takes a stream of a shared connection; HTTP/1.x: the connection, until the
            // response's body is read. Either gives up what it took, if it was canceled (HTTP/2's stream reserved for
            // it, too).
            tried  = leased->http2;
            result = co_await send_on(pool, std::move(*leased), request, how.first, dest->reusable);
            if (result.same_connection) tried = nullptr;

            // NOTE: a response that arrived as stop was requested is dropped, with its connection (HTTP/1.x) or
            //       stream.
            if (canceled()) co_return cancel();

            how.after(result);
        }
        while (result.retry);

        if (!result.response.has_value()) co_return std::move(result.response);

        http1_only        = how.http1_only;
        auto followed_now = follow(request, *result.response, limit, followed, http1_only);
        if (!followed_now.has_value()) co_return std::unexpected(followed_now.error());

        if (!*followed_now)
        {
            // NOTE: moved, as the request is done with: nothing refers to it once its response's head is in.
            result.response->url = std::move(request.uri);
            co_return std::move(result.response);
        }

        co_await discard(std::move(*result.response));
    }
}

coro::task<std::expected<client_response, std::error_code>> client::get(std::string_view url, std::stop_token stop)
{
    // NOTE: not a coroutine, so url is parsed before this returns, rather than when the task starts (when it might be
    //       gone).
    auto parsed = url::parse(url);
    if (!parsed.has_value())
    {
        return coro::ready_task<std::expected<client_response, std::error_code>>(
            std::unexpected(std::make_error_code(std::errc::invalid_argument)));
    }

    return send(client_request{.uri = std::move(parsed).value(), .stop = std::move(stop)});
}

void client::close_idle() noexcept { pool->close_idle(); }

connection_stats client::stats() const { return pool->stats(); }

connection_stats client::stats(const std::string& host, const std::string& port) const
{
    return pool->stats(host, port);
}

}
