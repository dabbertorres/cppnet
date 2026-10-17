#include "http/client.hpp"

#include <algorithm>
#include <chrono>
#include <compare>
#include <cstddef>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "coro/task.hpp"
#include "http/connection.hpp"
#include "http/headers.hpp"
#include "http/http.hpp"
#include "http/http11.hpp"
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
// read to its end (or closes, if it's destroyed before then).
class pooled_body final : public net::io::reader
{
public:
    pooled_body(std::shared_ptr<net::http::detail::connection_pool> pool,
                net::http::detail::connection_pool::lease           leased)
        : pool{std::move(pool)}
        , leased{std::move(leased)}
        , reader{std::make_unique<net::io::buffered_reader>(&net::http::detail::as_reader(*this->leased.conn))}
    {}

    pooled_body(const pooled_body&)            = delete;
    pooled_body& operator=(const pooled_body&) = delete;
    pooled_body(pooled_body&&)                 = delete;
    pooled_body& operator=(pooled_body&&)      = delete;

    ~pooled_body() override { finish(false); }

    [[nodiscard]] net::http::detail::connection* connection() const noexcept { return leased.conn.get(); }
    [[nodiscard]] net::io::buffered_reader* buffered() const noexcept { return reader.get(); }

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
        if (body == nullptr) co_return {.err = make_error_code(net::io::errc::end_of_stream)};

        auto res = co_await body->read(data);

        if (res.err)
        {
            finish(res.err == net::io::errc::end_of_stream);
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

}

namespace net::http
{

client::client(io::scheduler* scheduler, const client_options& options)
    : pool{std::make_shared<detail::connection_pool>(scheduler, options)}
{}

coro::task<std::expected<client_response, std::error_code>> client::send(client_request request)
{
    // Only HTTP/1.x is implemented: the http2 encoders are stubs.
    if (request.version.major > 1) co_return std::unexpected(std::make_error_code(std::errc::not_supported));

    auto target = endpoint(request.uri);
    if (!target.has_value()) co_return std::unexpected(target.error());
    const auto& [scheme, port] = *target;

    // before taking a connection, which a request that can't be sent would waste.
    if (auto err = http11::check_request(request); err) co_return std::unexpected(err);

    // a request that closes its connection can't be followed by another.
    const bool reusable_request = request.version.minor >= 1 && !has_close(request.headers);

    for (int attempt = 0;; ++attempt)
    {
        // a retry is because a reused connection had been closed, so the others may have been too.
        auto leased = co_await pool->acquire(scheme, request.uri.host, port, attempt == 0);
        if (!leased.has_value()) co_return std::unexpected(leased.error());

        const bool reused = leased->reused;
        auto       body   = std::make_unique<pooled_body>(pool, std::move(*leased));
        auto*      conn   = body->connection();

        // NOTE: set every time, as a reused connection has whatever its last request left.
        const auto& options = pool->options();
        auto&       sock    = detail::socket_of(*conn);
        sock.set_deadline(std::nullopt);
        sock.set_timeout(limit(options.io_timeout));

        // NOTE: not with a body, which has been read.
        const auto retry = [&](std::error_code err)
        {
            return reused && attempt == 0 && request.body == nullptr && is_idempotent(request.method)
                && closed_early(err);
        };

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
            if (retry(res.error())) continue;
            if (request.body == nullptr || !closed_early(res.error())) co_return std::unexpected(res.error());
        }

        if (auto wait = limit(options.response_header_timeout)) sock.set_read_deadline(io::clock::now() + *wait);

        auto resp = co_await http11::response_decode(body->buffered(), request.method, max_response_header_bytes);

        // the body's reads are only limited by io_timeout.
        sock.set_read_deadline(std::nullopt);
        if (!resp.has_value())
        {
            if (write_failed) co_return std::unexpected(res.error());
            if (retry(resp.error())) continue;
            co_return std::unexpected(resp.error());
        }

        // the server didn't get all of the request, so the connection is no use for another.
        const bool reuse =
            !write_failed && reusable_request && http11::connection_reusable(*resp, request.method);
        body->attach(std::move(resp->body), reuse, known_length(*resp, request.method));
        resp->body = std::move(body);

        co_return std::move(*resp);
    }
}

coro::task<std::expected<client_response, std::error_code>> client::get(std::string_view url)
{
    // NOTE: not a coroutine, so url is parsed before this returns, rather than when the task starts (when it might be
    //       gone).
    auto parsed = url::parse(url);
    if (!parsed.has_value())
    {
        return coro::ready_task<std::expected<client_response, std::error_code>>(
            std::unexpected(std::make_error_code(std::errc::invalid_argument)));
    }

    return send(client_request{.uri = std::move(parsed).value()});
}

void client::close_idle() noexcept { pool->close_idle(); }

connection_stats client::stats() const { return pool->stats(); }

connection_stats client::stats(const std::string& host, const std::string& port) const
{
    return pool->stats(host, port);
}

}
