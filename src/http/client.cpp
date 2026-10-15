#include "http/client.hpp"

#include <algorithm>
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
#include "http/http.hpp"
#include "http/http11.hpp"
#include "http/request.hpp"
#include "http/response.hpp"
#include "io/buffered_reader.hpp"
#include "io/buffered_writer.hpp"
#include "io/io.hpp"
#include "io/scheduler.hpp"
#include "tcp.hpp"
#include "util/string_util.hpp"

namespace
{

using namespace std::string_view_literals;

// a server's response head can't use up all of the client's memory.
constexpr std::size_t max_response_header_bytes = 1'024 * 1'024;

// pooled_body reads a response's body from a leased connection, which it returns to the pool once the body has been
// read to its end (or closes, if it's destroyed before then).
class pooled_body final : public net::io::reader
{
public:
    pooled_body(std::shared_ptr<net::http::detail::connection_pool> pool,
                net::http::detail::connection_pool::lease          leased)
        : pool{std::move(pool)}
        , leased{std::move(leased)}
        , reader{std::make_unique<net::io::buffered_reader>(this->leased.conn.get())}
    {}

    pooled_body(const pooled_body&)            = delete;
    pooled_body& operator=(const pooled_body&) = delete;
    pooled_body(pooled_body&&)                 = delete;
    pooled_body& operator=(pooled_body&&)      = delete;

    ~pooled_body() override { finish(false); }

    [[nodiscard]] net::tcp_socket*          connection() const noexcept { return leased.conn.get(); }
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
        if (body == nullptr) co_return {.err = make_error_condition(net::io::status_condition::closed)};

        auto res = co_await body->read(data);

        if (res.err)
        {
            finish(res.err == net::io::status_condition::closed);
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

    [[nodiscard]] int native_handle() const noexcept override
    {
        return leased.conn != nullptr ? leased.conn->native_handle() : -1;
    }

private:
    // finish returns the connection to the pool, which may reuse it if the whole body was read.
    void finish(bool complete) noexcept
    {
        if (leased.conn == nullptr) return;

        // nothing is pipelined, so anything after the body is the server misbehaving.
        const bool reuse = complete && reusable && reader->buffered().empty();

        body.reset();
        reader.reset();
        pool->release(std::move(leased), reuse);
    }

    std::shared_ptr<net::http::detail::connection_pool> pool;
    net::http::detail::connection_pool::lease           leased;
    std::unique_ptr<net::io::buffered_reader>           reader;
    std::unique_ptr<net::io::reader>                    body;
    bool                                                reusable = false;
    std::optional<std::size_t>                          remaining;
};

bool is_idempotent(net::http::request_method method) noexcept
{
    using enum net::http::request_method;
    return method == GET || method == HEAD || method == OPTIONS || method == TRACE || method == PUT || method == DELETE;
}

// closed_early is whether a request failed as it would on a connection the server had closed before getting it.
bool closed_early(std::error_condition err) noexcept
{
    return err == net::io::status_condition::closed || err == std::errc::connection_reset
        || err == std::errc::broken_pipe || err == std::errc::connection_aborted;
}

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

coro::task<std::expected<client_response, std::error_condition>> client::send(const client_request& request)
{
    // Only HTTP/1.x over TCP is implemented. In particular, the http2 encoders are stubs, and there's no TLS.
    if (request.version.major > 1) co_return std::unexpected(std::make_error_condition(std::errc::not_supported));
    if (!request.uri.scheme.empty() && !util::equal_ignore_case(std::string_view{request.uri.scheme}, "http"sv))
        co_return std::unexpected(std::make_error_condition(std::errc::protocol_not_supported));

    const auto port = !request.uri.port.empty() ? request.uri.port : std::string{"80"};

    // a request that closes its connection can't be followed by another.
    const bool reusable_request = request.version.minor >= 1 && !has_close(request.headers);

    for (int attempt = 0;; ++attempt)
    {
        // a retry is because a reused connection had been closed, so the others may have been too.
        auto leased = co_await pool->acquire(request.uri.host, port, attempt == 0);
        if (!leased.has_value()) co_return std::unexpected(leased.error());

        const bool reused = leased->reused;
        auto       body   = std::make_unique<pooled_body>(pool, std::move(*leased));

        const auto retry = [&](std::error_condition err)
        { return reused && attempt == 0 && is_idempotent(request.method) && closed_early(err); };

        // the head in one write, rather than one per field.
        io::buffered_writer out{body->connection(), 4'096};

        auto res = co_await http11::request_encode(&out, request);
        if (res.has_value())
        {
            if (auto flushed = co_await out.flush(); flushed.err) res = std::unexpected(flushed.err);
        }

        if (!res.has_value())
        {
            if (retry(res.error())) continue;
            co_return std::unexpected(res.error());
        }

        auto resp = co_await http11::response_decode(body->buffered(), request.method, max_response_header_bytes);
        if (!resp.has_value())
        {
            if (retry(resp.error())) continue;
            co_return std::unexpected(resp.error());
        }

        const bool reuse = reusable_request && http11::connection_reusable(*resp, request.method);
        body->attach(std::move(resp->body), reuse, known_length(*resp, request.method));
        resp->body = std::move(body);

        co_return std::move(*resp);
    }
}

void client::close_idle() noexcept { pool->close_idle(); }

connection_stats client::stats() const { return pool->stats(); }

connection_stats client::stats(const std::string& host, const std::string& port) const
{
    return pool->stats(host, port);
}

}
