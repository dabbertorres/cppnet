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
#include "http/request.hpp"
#include "http/response.hpp"
#include "io/buffered_reader.hpp"
#include "io/buffered_writer.hpp"
#include "io/io.hpp"
#include "io/reader.hpp"
#include "io/scheduler.hpp"
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
class pooled_body final : public net::io::reader
{
public:
    pooled_body(std::shared_ptr<net::http::detail::connection_pool> pool,
                net::http::detail::connection_pool::lease           leased,
                std::stop_token                                     stop)
        : pool{std::move(pool)}
        , leased{std::move(leased)}
        , reader{std::make_unique<net::io::buffered_reader>(&net::http::detail::as_reader(*this->leased.conn))}
        , stop{std::move(stop)}
    {}

    pooled_body(const pooled_body&)            = delete;
    pooled_body& operator=(const pooled_body&) = delete;
    pooled_body(pooled_body&&)                 = delete;
    pooled_body& operator=(pooled_body&&)      = delete;

    ~pooled_body() override { finish(false); }

    [[nodiscard]] net::http::detail::connection* connection() const noexcept { return leased.conn.get(); }
    [[nodiscard]] net::io::buffered_reader*      buffered() const noexcept { return reader.get(); }

    // attach sets the body that's read from the connection. length is how long it is, if it's known up front.
    void attach(std::unique_ptr<net::io::reader> decoded, bool reuse, std::optional<std::size_t> length)
    {
        body      = std::move(decoded);
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

        request.body     = nullptr;
        request.get_body = nullptr;
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
};

// send_http2 sends a request on a stream of an HTTP/2 connection. It may be sent again (on another connection):
// - whatever it is, if it was refused before anything of it was sent (the connection was ending);
// - whatever its method, if the server didn't process it (it was refused, or after a GOAWAY's last stream), unless its
//   body's been read;
// - if it's safe to repeat, and has no body, if the connection dropped (without GOAWAY) before its response's head: as
//   HTTP/1.1's are, when a reused connection turns out to have been closed.
net::coro::task<sent>
send_http2(net::http::detail::connection_pool::lease leased, net::http::client_request& request, bool first)
{
    net::http::detail::http2_client_connection::send_state state;

    auto resp = co_await leased.http2->send(request, &state);
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

    // the head in one write, rather than one per field.
    io::buffered_writer out{&detail::as_writer(*body->connection()), 4'096};

    auto res = co_await http11::request_encode(&out, request);
    if (res.has_value())
    {
        if (auto flushed = co_await out.flush(); flushed.err) res = std::unexpected(flushed.err);
    }

    // a server may respond before reading all of the body (e.g. 413 Content Too Large), and then close the
    // connection, failing the rest of the write. The response is still worth reading, if it's there.
    const bool write_failed = !res.has_value();
    if (write_failed)
    {
        if (retry(res.error())) co_return sent{.response = std::unexpected(res.error()), .retry = true};
        if (request.body == nullptr || !closed_early(res.error()))
            co_return sent{.response = std::unexpected(res.error())};
    }

    // NOTE: a deadline, for the final head: interim responses don't put it off.
    if (auto wait = limit(options.response_header_timeout)) sock.set_read_deadline(io::clock::now() + *wait);

    // an interim response says the server has the request, so it's not sent again if the connection then ends (as over
    // HTTP/2, which counts the stream as processed); the caller's told of each (an exception it throws closes the
    // connection, as the body's destroyed with this frame).
    bool       interim          = false;
    const auto on_informational = [&request, &interim](net::http::status code, const net::http::headers& fields)
    {
        interim = true;
        if (request.on_informational) request.on_informational(code, fields);
    };

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
    const bool reuse = !write_failed && reusable_request && http11::connection_reusable(*resp, request.method);
    body->attach(std::move(resp->body), reuse, known_length(*resp, request.method));
    resp->body = std::move(body);

    co_return sent{.response = std::move(*resp)};
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

    // each redirect is a request of its own: its connection, retries and timeouts.
    while (true)
    {
        const auto dest = destination_of(request);
        if (!dest.has_value()) co_return std::unexpected(dest.error());

        // the HTTP/2 connection the last attempt was on: a retry is on another.
        std::shared_ptr<detail::http2_client_connection> tried;
        sent                                             result;

        for (int attempt = 0; attempt == 0 || result.retry; ++attempt)
        {
            // a retry is because a reused connection had been closed, so the others may have been too.
            auto leased = co_await pool->acquire(dest->scheme,
                                                 request.uri.host,
                                                 dest->port,
                                                 attempt == 0,
                                                 tried.get(),
                                                 request.stop);
            if (!leased.has_value()) co_return fail(leased.error());

            // HTTP/2: the request takes a stream of a shared connection; HTTP/1.x: the connection, until the
            // response's body is read. Either gives up what it took, if it was canceled (HTTP/2's stream reserved for
            // it, too).
            tried  = leased->http2;
            result = co_await send_on(pool, std::move(*leased), request, attempt == 0, dest->reusable);

            // NOTE: a response that arrived as stop was requested is dropped, with its connection (HTTP/1.x) or
            //       stream.
            if (canceled()) co_return cancel();
        }

        if (!result.response.has_value()) co_return std::move(result.response);

        auto followed_now = redirect(request, *result.response, limit, followed);
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
