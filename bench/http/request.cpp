// bench_request measures what the server does for each request: decoding it (HTTP/1.1), routing it to a handler, the
// handler's response, and encoding that, with TechEmpower's plaintext requests. It counts heap allocations per request
// too. Four ways:
// - pipelined: 16 requests read together (as a client pipelining them sends them), from memory rather than a socket.
// - one per read: the same, but each read hands over just one request, and has to wait for it (a coroutine, as a
//   socket's read that has to wait is): as for a client that waits for each response, apart from the socket.
// - over a socket: the server itself, and a client on another thread sending a request at a time (as TechEmpower's
//   JSON test does), so with the socket, scheduler and kernel's round trip too. And again with a handler that waits
//   once (for another pool, as for a database's driver), whose connection the server watches meanwhile, to notice the
//   client leaving.
// - over HTTP/2: the same over cleartext HTTP/2 (prior knowledge), a stream at a time from cppnet's own client, whose
//   allocations are counted with the server's (as they share the process): for what a stream costs. And the bytes a
//   request and its response come to each way (frames, HPACK's blocks and all), counted by a relay between another
//   client and the server, in a run of their own: for what HPACK's dynamic table saves. And several streams at once,
//   each a larger response (written in pieces, as a file's served), so they contend for the connection: for what the
//   server's scheduling of their DATA costs (see docs/plans/http2.md, Priorities). And the other way: several request
//   bodies at once from cppnet's client, which contend for the connection in the client's scheduler
//   (docs/plans/http2.md, stage 6's seventh part).
// And pipelined again, through a chain of middleware (instrument::http_metrics) around the router: for what a chain
// costs (see docs/plans/middleware.md), with the metrics' own work; and with the metrics on the route instead (use()),
// with each router: for what middleware on routes costs. And through http::timeout on the route, pipelined and over a
// socket, with a deadline that never comes: for what its timer costs.
#include "http/request.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <memory>
#include <mutex>
#include <new>
#include <print>
#include <ratio>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <spdlog/common.h>
#include <spdlog/spdlog.h>

#include "coro/task.hpp"
#include "coro/thread_pool.hpp"
#include "http/client.hpp"
#include "http/connection_pool.hpp"
#include "http/http.hpp"
#include "http/http11.hpp"
#include "http/middleware.hpp"
#include "http/response.hpp"
#include "http/router.hpp"
#include "http/server.hpp"
#include "http/static_router.hpp"
#include "http/timeout.hpp"
#include "instrument/middleware.hpp"
#include "io/buffered_reader.hpp"
#include "io/buffered_writer.hpp"
#include "io/discard_writer.hpp"
#include "io/flush_before_read.hpp"
#include "io/io.hpp"
#include "io/reader.hpp"
#include "io/scheduler.hpp"
#include "io/string_reader.hpp"
#include "io/writer.hpp"
#include "url.hpp"

// NOLINTBEGIN(misc-new-delete-overloads,cppcoreguidelines-no-malloc,cppcoreguidelines-avoid-non-const-global-variables,readability-inconsistent-declaration-parameter-name):
// counting allocations, by replacing these.
namespace
{

// the heap allocations so far, on any thread.
std::atomic<std::size_t> allocations = 0;
}

void* operator new(std::size_t size)
{
    allocations.fetch_add(1, std::memory_order::relaxed);
    if (void* p = std::malloc(size)) return p;
    throw std::bad_alloc{};
}

void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t /*size*/) noexcept { std::free(p); }
// NOLINTEND(misc-new-delete-overloads,cppcoreguidelines-no-malloc,cppcoreguidelines-avoid-non-const-global-variables,readability-inconsistent-declaration-parameter-name)

namespace
{

namespace http = net::http;

using http::literals::operator""_route;
using namespace std::string_view_literals;
using namespace std::chrono_literals;
using net::coro::task;

// as bench/web/cppnet's, and wrk sends it, with TechEmpower's headers.
constexpr auto request =
    "GET /plaintext HTTP/1.1\r\n"
    "Host: localhost\r\n"
    "Accept: text/plain,text/html;q=0.9,application/xhtml+xml;q=0.9,application/xml;q=0.8,*/*;q=0.7\r\n"
    "Connection: keep-alive\r\n"
    "\r\n"sv;

// the same, to a handler that waits once.
constexpr auto waiting_request =
    "GET /waiting HTTP/1.1\r\n"
    "Host: localhost\r\n"
    "Accept: text/plain,text/html;q=0.9,application/xhtml+xml;q=0.9,application/xml;q=0.8,*/*;q=0.7\r\n"
    "Connection: keep-alive\r\n"
    "\r\n"sv;

constexpr auto body = "Hello, World!"sv;

constexpr std::size_t pipelined       = 16;
constexpr int         batches         = 50'000;
constexpr int         repeats         = 7;
constexpr std::size_t date_bytes      = 29; // e.g. "Sun, 06 Nov 1994 08:49:37 GMT"
constexpr std::size_t buffer_size     = std::size_t{16} * 1'024;
constexpr int         socket_requests = 50'000;
constexpr int         http2_requests  = 20'000;
constexpr int         counted_bytes   = 1'000; // requests

// the contending streams' responses: contending at once, a round of them at a time.
constexpr std::size_t bulk_size        = std::size_t{256} * 1'024;
constexpr std::size_t bulk_write       = std::size_t{64} * 1'024;
constexpr int         contending       = 8;
constexpr int         contended_rounds = 300;

// the contending uploads' bodies: as the responses' (bulk_size each, contending at once), the other way.
constexpr std::size_t upload_size = bulk_size;

task<void> plaintext(const http::server_request& /*req*/, http::response_writer& resp)
{
    resp.headers().set("Server"sv, "cppnet"sv).set("Content-Type"sv, "text/plain"sv);
    auto* out = co_await resp.send(http::status::OK, body.size());
    co_await net::io::write_all(*out, body);
}

// bulk answers with bulk_size bytes, written bulk_write at a time (as a file's served).
task<void> bulk(const http::server_request& /*req*/, http::response_writer& resp)
{
    static const std::string piece(bulk_write, 'b');

    resp.headers().set("Content-Type"sv, "application/octet-stream"sv);
    auto* out = co_await resp.send(http::status::OK, bulk_size);
    for (std::size_t sent = 0; sent < bulk_size; sent += bulk_write) co_await net::io::write_all(*out, piece);
}

// consume reads a request's body to its end (16 KiB at a time, as a file's written), and answers with an empty 200 if
// it was upload_size bytes (a 400 otherwise).
task<void> consume(const http::server_request& req, http::response_writer& resp)
{
    std::array<std::byte, std::size_t{16} * 1'024> scratch{};
    std::size_t                                    read = 0;
    while (true)
    {
        const auto res = co_await req.body->read(scratch);
        read += res.count;
        if (res.err) break;
    }
    co_await resp.send(read == upload_size ? http::status::OK : http::status::BAD_REQUEST, 0);
}

// waits_once answers as plaintext does, after waiting once: for a turn on another pool.
struct waits_once
{
    net::coro::thread_pool* elsewhere;

    task<void> operator()(const http::server_request& req, http::response_writer& resp) const
    {
        co_await elsewhere->yield();
        co_await plaintext(req, resp);
    }
};

// one_request_per_read hands over its input a request at a time, each through a coroutine (as a socket's read that has
// to wait for data is).
class one_request_per_read final : public net::io::reader
{
public:
    explicit one_request_per_read(std::string_view input) noexcept
        : input{input}
    {}

    task<net::io::result> read(std::span<std::byte> data) override { return wait_then_read(data); }

    using net::io::reader::read;

    [[nodiscard]] net::io::handle native_handle() const noexcept override { return -1; }

private:
    task<net::io::result> wait_then_read(std::span<std::byte> data)
    {
        if (input.empty()) co_return net::io::result{.err = make_error_code(net::io::errc::end_of_stream)};

        // the rest of the current request, or as much as fits.
        const auto next = std::min({data.size(), input.size(), request.size() - (offset % request.size())});
        std::ranges::copy(std::as_bytes(std::span{input.substr(0, next)}), data.begin());
        input.remove_prefix(next);
        offset += next;

        co_return net::io::result{.count = next};
    }

    std::string_view input;
    std::size_t      offset = 0;
};

// steps counts allocations at each step of serving a request.
struct steps
{
    std::size_t decode   = 0;
    std::size_t handle   = 0; // routing, the handler, and encoding its response
    std::size_t finish   = 0; // the rest: e.g. the server's own headers, and skipping the request's body
    std::size_t requests = 0;
};

// serve serves the requests from input, as server::serve_connection and serve_request do (without the logging and
// error handling).
template<typename Router>
task<void> serve(const Router& router, net::io::reader& input, net::io::writer& out, steps& counted)
{
    net::io::buffered_writer   writer{&out, buffer_size};
    net::io::flush_before_read flushing{&input, &writer};
    net::io::buffered_reader   reader{&flushing, buffer_size};

    const std::string date(date_bytes, 'x');

    // kept for the connection, as the server does.
    http::server_request  req;
    http::server_response resp;

    while (true)
    {
        auto at = allocations.load(std::memory_order::relaxed);

        if (auto err = co_await http::http11::request_decode(&reader, 8'192, req); err) break;

        // as the server does (none, here).
        if (http::detail::expectation_of(req) != http::detail::expectation::none) break;

        counted.decode += allocations.load(std::memory_order::relaxed) - at;
        at = allocations.load(std::memory_order::relaxed);

        resp.version     = req.version;
        resp.status_code = http::status::NONE;
        resp.body        = &writer;
        resp.headers.clear();
        resp.headers.set("Date"sv, std::string_view{date});

        http::response_writer rw{&writer, &resp, http::http11::response_encode};

        counted.finish += allocations.load(std::memory_order::relaxed) - at;
        at = allocations.load(std::memory_order::relaxed);

        co_await router(req, rw);

        counted.handle += allocations.load(std::memory_order::relaxed) - at;
        at = allocations.load(std::memory_order::relaxed);

        // skips what's left of the body (none, here).
        std::array<std::byte, 64> scratch{};
        while (!(co_await req.body->read(scratch)).err)
        {}

        counted.finish += allocations.load(std::memory_order::relaxed) - at;
        ++counted.requests;
    }

    co_await writer.flush();
}

// measure serves batches of requests from memory, with make_input making the reader for each batch's input, and prints
// the time and allocations per request.
template<typename Router, typename MakeInput>
void measure(std::string_view name, const Router& router, MakeInput make_input)
{
    std::string input;
    for (std::size_t i = 0; i < pipelined; ++i) input += request;

    net::io::discard_writer out;
    std::vector<double>     runs;

    for (int r = 0; r < repeats; ++r)
    {
        steps counted;
        auto  start = std::chrono::steady_clock::now();

        for (int i = 0; i < batches; ++i)
        {
            auto in     = make_input(input);
            auto served = serve(router, in, out, counted);
            served.get_handle().resume();
        }

        auto took = std::chrono::steady_clock::now() - start;
        if (counted.requests != pipelined * batches) std::println(stderr, "served {} requests", counted.requests);

        runs.push_back(std::chrono::duration<double, std::nano>(took).count() / static_cast<double>(counted.requests));
    }

    std::ranges::sort(runs);

    // and once more, counting allocations at each step (the first request of a batch's are spread over the 16).
    steps counted;
    auto  in     = make_input(input);
    auto  served = serve(router, in, out, counted);
    served.get_handle().resume();

    const auto per_request = [&](std::size_t n)
    { return static_cast<double>(n) / static_cast<double>(counted.requests); };

    std::println("{}:", name);
    std::println("{:>10.1f} ns per request (best of {})", runs.front(), repeats);
    std::println("{:>10.1f} allocations per request: decoding {:.1f}, handling {:.1f}, the rest {:.1f}",
                 per_request(counted.decode + counted.handle + counted.finish),
                 per_request(counted.decode),
                 per_request(counted.handle),
                 per_request(counted.finish));
}

// client is a blocking client, which sends a request at a time.
class client
{
public:
    client(std::uint16_t port, std::string_view request)
        : fd{::socket(AF_INET, SOCK_STREAM, 0)}
        , request{request}
    {
        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_port        = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

        if (fd < 0)
        {
            std::println(stderr, "failed to make a socket");
            return;
        }

        // NOTE: the server listens from when it's constructed.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the sockets API.
        if (::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0)
            std::println(stderr, "failed to connect to the server");
    }

    client(const client&)            = delete;
    client& operator=(const client&) = delete;
    client(client&&)                 = delete;
    client& operator=(client&&)      = delete;

    ~client() { ::close(fd); }

    // round_trip sends a request, and reads its response (which ends with the body), without allocating.
    bool round_trip()
    {
        if (::send(fd, request.data(), request.size(), 0) != static_cast<ssize_t>(request.size())) return false;

        std::size_t got = 0;
        while (true)
        {
            auto n = ::recv(fd, buf.data() + got, buf.size() - got, 0);
            if (n <= 0) return false;
            got += static_cast<std::size_t>(n);

            if (std::string_view{buf.data(), got}.ends_with(body)) return true;
        }
    }

private:
    int                     fd;
    std::string_view        request;
    std::array<char, 4'096> buf{};
};

// measure_socket serves requests (each request) over a socket, a request at a time, and prints the time and allocations
// per request.
template<typename MakeRouter>
void measure_socket(std::string_view name, MakeRouter make_router, std::string_view request)
{
    auto               workers = std::make_shared<net::coro::thread_pool>(2);
    net::io::scheduler scheduler{workers};

    http::server server{
        &scheduler,
        make_router(&scheduler),
        {.host = "127.0.0.1", .port = "0"}
    };

    scheduler.spawn(server.serve());
    std::thread runner{[&] { scheduler.run(); }};

    {
        client conn{server.local_port(), request};

        // warms up: e.g. the connection's buffers, and its request and response, which are kept for it.
        for (int i = 0; i < 1'000; ++i)
        {
            if (!conn.round_trip()) std::println(stderr, "warm-up request failed");
        }

        const auto at    = allocations.load();
        const auto start = std::chrono::steady_clock::now();

        for (int i = 0; i < socket_requests; ++i)
        {
            if (!conn.round_trip())
            {
                std::println(stderr, "request {} failed", i);
                break;
            }
        }

        const auto took    = std::chrono::steady_clock::now() - start;
        const auto counted = allocations.load() - at;

        std::println("{} ({} workers):", name, workers->concurrency());
        std::println("{:>10.1f} us per request",
                     std::chrono::duration<double, std::micro>(took).count() / socket_requests);
        std::println("{:>10.1f} allocations per request",
                     static_cast<double>(counted) / static_cast<double>(socket_requests));
    }

    server.close();
    scheduler.shutdown();
    runner.join();
}

// loopback is 127.0.0.1's port.
sockaddr_in loopback(std::uint16_t port) noexcept
{
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return addr;
}

// NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast): the sockets API.
sockaddr* as_sockaddr(sockaddr_in& addr) noexcept { return reinterpret_cast<sockaddr*>(&addr); }

// NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)

// relay forwards the connections made to it to a port of 127.0.0.1's, counting the bytes each way: for what a request
// and its response come to on the wire. Blocking, with a thread each way per connection, so it's only used for the run
// that counts bytes, not the timed one.
class relay
{
public:
    explicit relay(std::uint16_t to)
        : to{to}
        , listener{::socket(AF_INET, SOCK_STREAM, 0)}
    {
        auto      addr   = loopback(0);
        socklen_t length = sizeof(addr);
        if (listener < 0 || ::bind(listener, as_sockaddr(addr), sizeof(addr)) != 0 || ::listen(listener, 8) != 0
            || ::getsockname(listener, as_sockaddr(addr), &length) != 0)
        {
            std::println(stderr, "failed to make the relay's listener");
            return;
        }

        listening = ntohs(addr.sin_port);
        acceptor  = std::thread{[this] { accept_connections(); }};
    }

    relay(const relay&)            = delete;
    relay& operator=(const relay&) = delete;
    relay(relay&&)                 = delete;
    relay& operator=(relay&&)      = delete;

    ~relay()
    {
        stopping = true;
        if (acceptor.joinable()) acceptor.join();

        // ends the forwarders' reads, whether or not the ends have closed.
        for (const auto fd : fds) ::shutdown(fd, SHUT_RDWR);
        for (auto& t : forwarders) t.join();
        for (const auto fd : fds) ::close(fd);
        if (listener >= 0) ::close(listener);
    }

    [[nodiscard]] std::uint16_t port() const noexcept { return listening; }

    // up is what the clients have sent, and down what the server has.
    [[nodiscard]] std::size_t up() const noexcept { return sent_up.load(); }
    [[nodiscard]] std::size_t down() const noexcept { return sent_down.load(); }

private:
    void accept_connections()
    {
        while (!stopping)
        {
            pollfd p{.fd = listener, .events = POLLIN, .revents = 0};
            if (::poll(&p, 1, 50) <= 0) continue;

            const int from = ::accept(listener, nullptr, nullptr);
            if (from < 0) continue;

            auto      addr = loopback(to);
            const int dest = ::socket(AF_INET, SOCK_STREAM, 0);
            if (dest < 0 || ::connect(dest, as_sockaddr(addr), sizeof(addr)) != 0)
            {
                std::println(stderr, "the relay failed to connect to the server");
                ::close(from);
                if (dest >= 0) ::close(dest);
                continue;
            }

            // NOTE: what's read is written at once, as the ends wrote it: Nagle's algorithm would hold small frames
            //       back for the peer's delayed ACK.
            const int on = 1;
            ::setsockopt(from, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
            ::setsockopt(dest, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));

            std::scoped_lock lock{mu};
            fds.push_back(from);
            fds.push_back(dest);
            forwarders.emplace_back([this, from, dest] { forward(from, dest, sent_up); });
            forwarders.emplace_back([this, from, dest] { forward(dest, from, sent_down); });
        }
    }

    // write_all writes all of data to fd, returning whether it could.
    static bool write_all(int fd, std::span<const char> data)
    {
        while (!data.empty())
        {
            const auto n = ::write(fd, data.data(), data.size());
            if (n <= 0) return false;
            data = data.subspan(static_cast<std::size_t>(n));
        }
        return true;
    }

    // forward copies what's read from in to out, until in ends (then ending out's writing side) or out fails.
    static void forward(int in, int out, std::atomic<std::size_t>& count)
    {
        std::array<char, 16'384> buf{};
        while (true)
        {
            const auto n = ::read(in, buf.data(), buf.size());
            if (n <= 0) break;

            count += static_cast<std::size_t>(n);
            if (!write_all(out, std::span{buf.data(), static_cast<std::size_t>(n)})) break;
        }

        ::shutdown(out, SHUT_WR);
    }

    std::uint16_t            to;
    int                      listener;
    std::uint16_t            listening = 0;
    std::atomic<bool>        stopping  = false;
    std::atomic<std::size_t> sent_up   = 0;
    std::atomic<std::size_t> sent_down = 0;
    std::thread              acceptor;

    std::mutex               mu; // for fds and forwarders, until acceptor has been joined
    std::vector<int>         fds;
    std::vector<std::thread> forwarders;
};

// http2_outcome is what measure_http2's client measured.
struct http2_outcome
{
    std::size_t              allocations = 0;
    std::chrono::nanoseconds took{};
    int                      failed = -1; // the request that failed, if one did
};

// get_all GETs url, and reads its response's body to the end, returning whether it got all of it.
task<bool> get_all(http::client& client, std::string_view url)
{
    auto resp = co_await client.get(url);
    if (!resp.has_value()) co_return false;

    std::array<std::byte, 64> scratch{};
    while (true)
    {
        auto res = co_await resp->body->read(scratch);
        if (res.err) co_return res.err == net::io::errc::end_of_stream;
    }
}

// send_http2_requests sends requests to url one at a time, after warming up, counting the allocations meanwhile.
task<http2_outcome> send_http2_requests(http::client* client, std::string url)
{
    // warms up: e.g. the connection, and the session's buffers.
    for (int i = 0; i < 1'000; ++i)
    {
        if (!co_await get_all(*client, url)) co_return http2_outcome{.failed = i};
    }

    http2_outcome out;
    const auto    at    = allocations.load();
    const auto    start = std::chrono::steady_clock::now();

    for (int i = 0; i < http2_requests; ++i)
    {
        if (!co_await get_all(*client, url))
        {
            out.failed = i;
            break;
        }
    }

    out.took        = std::chrono::steady_clock::now() - start;
    out.allocations = allocations.load() - at;
    co_return out;
}

// contention is what the contending requests share: how many have finished, and how many failed (or came short).
struct contention
{
    std::atomic<std::uint64_t> finished{0};
    std::atomic<int>           failed{0};
};

// fetch GETs url, and reads its response's body to the end, counting it finished (and failed, if it isn't all
// bulk_size bytes).
task<void> fetch(net::io::scheduler* scheduler, http::client* client, std::string_view url, contention* counts)
{
    std::size_t read = 0;
    if (auto resp = co_await client->get(url); resp.has_value())
    {
        std::vector<std::byte> scratch(std::size_t{16} * 1'024);
        while (true)
        {
            auto res = co_await resp->body->read(scratch);
            read += res.count;
            if (res.err) break;
        }
    }

    if (read != bulk_size) counts->failed.fetch_add(1);
    counts->finished.fetch_add(1);
    scheduler->notify_all(counts->finished);
}

// upload POSTs upload_size bytes to url (with their Content-Length), and reads its response to the end, counting it
// finished (and failed, if it isn't a 200).
task<void> upload(net::io::scheduler* scheduler, http::client* client, std::string_view url, contention* counts)
{
    static const std::string payload(upload_size, 'u');

    http::client_request req{.method = http::request_method::POST, .uri = net::url::parse(url).value()};
    req.headers.set_content_length(upload_size);
    req.body = std::make_unique<net::io::string_reader<char>>(payload);

    bool ok = false;
    if (auto resp = co_await client->send(std::move(req)); resp.has_value())
    {
        std::array<std::byte, 64> scratch{};
        while (true)
        {
            const auto res = co_await resp->body->read(scratch);
            if (res.err) break;
        }
        ok = resp->status_code == http::status::OK;
    }

    if (!ok) counts->failed.fetch_add(1);
    counts->finished.fetch_add(1);
    scheduler->notify_all(counts->finished);
}

// contender is a request that contends with others: fetch, or upload.
using contender = task<void> (*)(net::io::scheduler*, http::client*, std::string_view, contention*);

// contend_round sends contending requests to url at once, and waits for them, returning whether they all got their
// responses.
task<bool> contend_round(
    net::io::scheduler* scheduler, http::client* client, std::string_view url, contention* counts, contender send)
{
    const auto want = counts->finished.load() + contending;
    for (int i = 0; i < contending; ++i)
    {
        if (!scheduler->spawn(send(scheduler, client, url, counts))) co_return false;
    }
    for (auto seen = counts->finished.load(); seen < want; seen = counts->finished.load())
        (void)co_await scheduler->wait_on(counts->finished, seen);
    co_return counts->failed.load() == 0;
}

// send_contending sends contending requests to url at once, a round at a time (after warming up), counting the
// allocations meanwhile.
task<http2_outcome>
send_contending(net::io::scheduler* scheduler, http::client* client, std::string url, contender send)
{
    contention counts;
    for (int i = 0; i < 20; ++i)
    {
        if (!co_await contend_round(scheduler, client, url, &counts, send)) co_return http2_outcome{.failed = i};
    }

    http2_outcome out;
    const auto    at    = allocations.load();
    const auto    start = std::chrono::steady_clock::now();

    for (int i = 0; i < contended_rounds; ++i)
    {
        if (!co_await contend_round(scheduler, client, url, &counts, send))
        {
            out.failed = i;
            break;
        }
    }

    out.took        = std::chrono::steady_clock::now() - start;
    out.allocations = allocations.load() - at;
    co_return out;
}

// measure_contended serves contending streams over cleartext HTTP/2 (one connection, from cppnet's client), and prints
// the time a round of them takes, and the allocations per response (the client's and the server's together).
template<typename Router>
void measure_contended(const Router& router)
{
    auto               workers = std::make_shared<net::coro::thread_pool>(2);
    net::io::scheduler scheduler{workers};

    http::server server{
        &scheduler,
        router,
        {.host = "127.0.0.1", .port = "0"}
    };
    scheduler.spawn(server.serve());

    http::client_options options;
    options.http2_prior_knowledge = true;
    http::client client{&scheduler, options};

    const auto url = std::format("http://127.0.0.1:{}/bulk", server.local_port());
    const auto out = scheduler.run_to_completion(send_contending(&scheduler, &client, url, fetch));
    if (out.failed >= 0) std::println(stderr, "a contending HTTP/2 round {} failed", out.failed);

    const auto per_round = std::chrono::duration<double, std::micro>(out.took).count() / contended_rounds;
    const auto bytes     = static_cast<double>(bulk_size) * contending;
    std::println("over HTTP/2, {} streams at once ({} KiB each, written {} KiB at a time; {} workers):",
                 contending,
                 bulk_size / 1'024,
                 bulk_write / 1'024,
                 workers->concurrency());
    std::println("{:>10.1f} us per round ({:.0f} MiB/s)", per_round, bytes / per_round * 1e6 / (1'024.0 * 1'024.0));
    std::println("{:>10.1f} allocations per response",
                 static_cast<double>(out.allocations) / (static_cast<double>(contended_rounds) * contending));

    server.close();
    scheduler.shutdown();
}

// measure_uploads sends contending request bodies over cleartext HTTP/2 (one connection, from cppnet's client, whose
// scheduler orders them), and prints the time a round of them takes, and the allocations per request (the client's and
// the server's together).
template<typename Router>
void measure_uploads(const Router& router)
{
    auto               workers = std::make_shared<net::coro::thread_pool>(2);
    net::io::scheduler scheduler{workers};

    http::server server{
        &scheduler,
        router,
        {.host = "127.0.0.1", .port = "0"}
    };
    scheduler.spawn(server.serve());

    http::client_options options;
    options.http2_prior_knowledge = true;
    http::client client{&scheduler, options};

    const auto url = std::format("http://127.0.0.1:{}/upload", server.local_port());
    const auto out = scheduler.run_to_completion(send_contending(&scheduler, &client, url, upload));
    if (out.failed >= 0) std::println(stderr, "a contending HTTP/2 upload round {} failed", out.failed);

    const auto per_round = std::chrono::duration<double, std::micro>(out.took).count() / contended_rounds;
    const auto bytes     = static_cast<double>(upload_size) * contending;
    std::println("over HTTP/2, {} request bodies at once ({} KiB each, read 16 KiB at a time; {} workers):",
                 contending,
                 upload_size / 1'024,
                 workers->concurrency());
    std::println("{:>10.1f} us per round ({:.0f} MiB/s)", per_round, bytes / per_round * 1e6 / (1'024.0 * 1'024.0));
    std::println("{:>10.1f} allocations per request",
                 static_cast<double>(out.allocations) / (static_cast<double>(contended_rounds) * contending));

    server.close();
    scheduler.shutdown();
}

// http2_bytes is what count_http2_bytes counted: the bytes each way (up from the client, down from the server).
struct http2_bytes
{
    std::size_t first_up    = 0; // the first request's, with the connection's preface and SETTINGS
    std::size_t first_down  = 0;
    std::size_t second_up   = 0;
    std::size_t second_down = 0;
    std::size_t up          = 0; // counted_bytes requests' after those
    std::size_t down        = 0;
    int         failed      = -1;
};

// settle waits a moment: after a response, for what the client sends after it (e.g. a WINDOW_UPDATE).
task<void> settle(net::io::scheduler* scheduler)
{
    const auto err = co_await scheduler->sleep_until(net::io::clock::now() + 20ms);
    if (err) std::println(stderr, "failed to wait: {}", err.message());
}

// count_http2_bytes sends requests to url one at a time, through via, counting the bytes each way: of the first
// request, of the second, and of counted_bytes more.
task<http2_bytes> count_http2_bytes(net::io::scheduler* scheduler, http::client* client, std::string url, relay* via)
{
    http2_bytes out;

    if (!co_await get_all(*client, url)) co_return http2_bytes{.failed = 0};
    co_await settle(scheduler);
    out.first_up   = via->up();
    out.first_down = via->down();

    if (!co_await get_all(*client, url)) co_return http2_bytes{.failed = 1};
    co_await settle(scheduler);
    out.second_up   = via->up() - out.first_up;
    out.second_down = via->down() - out.first_down;

    const auto up_at   = via->up();
    const auto down_at = via->down();
    for (int i = 0; i < counted_bytes; ++i)
    {
        if (!co_await get_all(*client, url))
        {
            out.failed = i + 2;
            co_return out;
        }
    }

    co_await settle(scheduler);
    out.up   = via->up() - up_at;
    out.down = via->down() - down_at;
    co_return out;
}

// measure_http2 serves requests over cleartext HTTP/2, a stream at a time from cppnet's client, and prints the time and
// allocations per request (the client's and the server's together); then the bytes a request and its response come
// to each way, through a relay (with another client, so on a connection of its own).
template<typename Router>
void measure_http2(const Router& router)
{
    auto               workers = std::make_shared<net::coro::thread_pool>(2);
    net::io::scheduler scheduler{workers};

    http::server server{
        &scheduler,
        router,
        {.host = "127.0.0.1", .port = "0"}
    };
    scheduler.spawn(server.serve());

    http::client_options options;
    options.http2_prior_knowledge = true;
    http::client client{&scheduler, options};

    // NOTE: the relay's client is another, so it makes a connection of its own, through the relay.
    relay        via{server.local_port()};
    http::client counting{&scheduler, options};

    const auto url          = std::format("http://127.0.0.1:{}/plaintext", server.local_port());
    const auto relayed      = std::format("http://127.0.0.1:{}/plaintext", via.port());
    const auto [out, bytes] = scheduler.run_to_completion(
        [](net::io::scheduler* scheduler,
           http::client*       client,
           std::string         url,
           http::client*       counting,
           std::string         relayed,
           relay*              via) -> task<std::pair<http2_outcome, http2_bytes>>
        {
            auto timed = co_await send_http2_requests(client, std::move(url));
            auto bytes = co_await count_http2_bytes(scheduler, counting, std::move(relayed), via);
            co_return std::pair{timed, bytes};
        }(&scheduler, &client, url, &counting, relayed, &via));
    if (out.failed >= 0) std::println(stderr, "HTTP/2 request {} failed", out.failed);
    if (bytes.failed >= 0) std::println(stderr, "HTTP/2 request {} through the relay failed", bytes.failed);

    std::println("over HTTP/2 (a stream at a time, {} workers; the client's allocations too):", workers->concurrency());
    std::println("{:>10.1f} us per request",
                 std::chrono::duration<double, std::micro>(out.took).count() / http2_requests);
    std::println("{:>10.1f} allocations per request",
                 static_cast<double>(out.allocations) / static_cast<double>(http2_requests));
    std::println("{:>10} bytes up, {} down: the first request (with the connection's preface and SETTINGS)",
                 bytes.first_up,
                 bytes.first_down);
    std::println("{:>10} bytes up, {} down: the second", bytes.second_up, bytes.second_down);
    std::println("{:>10.1f} bytes up, {:.1f} down per request, over {} more",
                 static_cast<double>(bytes.up) / counted_bytes,
                 static_cast<double>(bytes.down) / counted_bytes,
                 counted_bytes);

    server.close();
    scheduler.shutdown();
}

}

int main()
{
    spdlog::set_level(spdlog::level::warn);

    net::coro::thread_pool elsewhere{1};

    const auto router  = http::make_static_router(http::GET("/plaintext"_route, plaintext));
    const auto waiting = http::make_static_router(http::GET("/waiting"_route, waits_once{&elsewhere}));

    measure("pipelined", router, [](std::string_view input) { return net::io::string_reader{input}; });
    measure("one per read", router, [](std::string_view input) { return one_request_per_read{input}; });
    measure_socket(
        "over a socket, a request at a time",
        [&](net::io::scheduler* /*sched*/) { return router; },
        request);
    measure_socket(
        "over a socket, to a handler that waits once",
        [&](net::io::scheduler* /*sched*/) { return waiting; },
        waiting_request);
    measure_http2(router);
    measure_contended(http::make_static_router(http::GET("/bulk"_route, bulk)));
    measure_uploads(http::make_static_router(http::POST("/upload"_route, consume)));
    measure("pipelined, through http_metrics",
            http::chain(net::instrument::http_metrics{}, router),
            [](std::string_view input) { return net::io::string_reader{input}; });

    // NOTE: a lambda rather than plaintext itself, whose pointer would be called indirectly: its frame not elided.
    const auto on_route = http::make_static_router(
        http::use(net::instrument::http_metrics{}),
        http::GET("/plaintext"_route,
                  [](const http::server_request& req, http::response_writer& resp) { return plaintext(req, resp); }));
    measure("pipelined, http_metrics on the route",
            on_route,
            [](std::string_view input) { return net::io::string_reader{input}; });

    http::router runtime;
    runtime.GET("/plaintext", plaintext);
    measure("pipelined, the runtime router",
            runtime,
            [](std::string_view input) { return net::io::string_reader{input}; });

    runtime.use(net::instrument::http_metrics{});
    measure("pipelined, the runtime router, http_metrics on the route",
            runtime,
            [](std::string_view input) { return net::io::string_reader{input}; });

    // NOTE: a deadline that never comes, for what the timer costs (on the route, so the layer's frame is elided):
    //       against "pipelined" and "over a socket, a request at a time", which have the same handler.
    net::io::scheduler timers{std::make_shared<net::coro::thread_pool>(2)};

    const auto timed = [](net::io::scheduler* sched)
    {
        return http::make_static_router(http::use(http::timeout{sched, 60s}),
                                        http::GET("/plaintext"_route,
                                                  [](const http::server_request& req, http::response_writer& resp)
                                                  { return plaintext(req, resp); }));
    };
    measure("pipelined, http::timeout on the route",
            timed(&timers),
            [](std::string_view input) { return net::io::string_reader{input}; });
    measure_socket("over a socket, http::timeout on the route", timed, request);
}
