// bench_client measures cppnet's client against cppnet's server, over loopback: the time, the process's CPU time (user
// and system) and the heap allocations (the client's and the server's together, as they share the process) per
// request.
//
// By default, HTTP/1.1, for each size of the client's response reader (client_options::read_buffer_size: 1, 4 and
// 16 KiB), three ways:
// - small: GETs of a short response (TechEmpower's plaintext), one at a time;
// - long head lines: the same, but its response's head has four Set-Cookie fields of 1.5 KiB each, which a reader
//   smaller than a line reads apart, into a string;
// - a chunked download (8 MiB, the server writing 64 KiB at a time), which the caller reads 256 bytes at a time: from
//   the reader's buffer, which is refilled a buffer's worth at a time.
//
// `bench_client stream <client's window> <server's window> [MiB]` measures one HTTP/2 stream instead (cleartext, prior
// knowledge): a download of that many MiB (64 by default), and an upload of as many, with the client's and the server's
// stream windows (client_options and server_config's http2_stream_window) as given. One stream goes at most a window a
// round trip, so over loopback the windows hardly matter: it's for a link with a round trip (e.g. Linux's netem on the
// loopback interface).
#include "http/client.hpp"

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
#include <new>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <spdlog/common.h>
#include <spdlog/spdlog.h>

#include "coro/task.hpp"
#include "coro/thread_pool.hpp"
#include "cpu_time.hpp"
#include "http/http.hpp"
#include "http/request.hpp"
#include "http/response.hpp"
#include "http/router.hpp"
#include "http/server.hpp"
#include "io/io.hpp"
#include "io/scheduler.hpp"
#include "io/string_reader.hpp"
#include "io/util.hpp"
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

using namespace std::string_view_literals;
using net::coro::task;

constexpr int repeats = 5;

constexpr auto        body          = "Hello, World!"sv;
constexpr std::size_t cookie_size   = 1'536;
constexpr int         cookies       = 4;
constexpr std::size_t download_size = std::size_t{8} << 20;
constexpr std::size_t write_size    = std::size_t{64} * 1'024;
constexpr std::size_t piece_size    = 256;

constexpr std::array reader_sizes{std::size_t{1'024}, std::size_t{4'096}, std::size_t{16'384}};

// a case: its path, how many requests a run sends, and the size of each response's body.
struct load
{
    std::string_view name;
    std::string_view path;
    int              requests;
    std::size_t      size;
};

constexpr std::array loads{
    load{                                             .name = "small",.path = "/plaintext",.requests = 20'000,.size = body.size()                                                                                                                },
    load{                                   .name = "long head lines",   .path = "/cookies", .requests = 20'000, .size = body.size()},
    load{.name     = "chunked download (8 MiB, read 256 B at a time)",
         .path     = "/download",
         .requests = 20,
         .size     = download_size                                                                                                  },
};

task<void> plaintext(const http::server_request& /*req*/, http::response_writer& resp)
{
    resp.headers().set("Content-Type"sv, "text/plain"sv);
    auto* out = co_await resp.send(http::status::OK, body.size());
    co_await net::io::write_all(*out, body);
}

// with_cookies answers as plaintext, with cookies Set-Cookie fields of cookie_size bytes each.
task<void> with_cookies(const http::server_request& req, http::response_writer& resp)
{
    static const std::string value(cookie_size, 'c');
    for (int i = 0; i < cookies; ++i) resp.headers().add("Set-Cookie"sv, std::string_view{value});
    co_await plaintext(req, resp);
}

// download answers with download_size bytes, chunked (no Content-Length), written write_size at a time.
task<void> download(const http::server_request& /*req*/, http::response_writer& resp)
{
    static const std::string piece(write_size, 'd');

    auto* out = co_await resp.send(http::status::OK);
    for (std::size_t sent = 0; sent < download_size; sent += write_size) co_await net::io::write_all(*out, piece);
}

// upload reads a request's body to its end, and answers with an empty 200.
task<void> upload(const http::server_request& req, http::response_writer& resp)
{
    std::vector<std::byte> scratch(std::size_t{64} * 1'024);
    while (!(co_await req.body->read(scratch)).err)
    {}
    co_await resp.send(http::status::OK, 0);
}

// get_all GETs url, and reads its response's body to the end piece_size bytes at a time, returning how much it read
// (none if it failed).
task<std::size_t> get_all(http::client& client, std::string url)
{
    auto resp = co_await client.get(url);
    if (!resp.has_value()) co_return 0;

    std::array<std::byte, piece_size> piece{};
    std::size_t                       read = 0;
    while (true)
    {
        auto res = co_await resp->body->read(piece);
        read += res.count;
        if (res.err) co_return res.err == net::io::errc::end_of_stream ? read : 0;
    }
}

// measured is a run's: how long it took, the CPU time it used, and the allocations it made.
struct measured
{
    std::chrono::steady_clock::duration took{};
    bench::cpu_time                     cpu{};
    std::size_t                         allocations = 0;
    bool                                failed      = false;
};

// run sends l's requests one at a time, after warming up, and returns the best of repeats runs (the least time, and the
// least CPU time, each of its own run).
task<measured> run(http::client* client, std::string url, load l)
{
    for (int i = 0; i < std::min(l.requests, 100); ++i)
    {
        if (co_await get_all(*client, url) != l.size) co_return measured{.failed = true};
    }

    measured        best{.took = std::chrono::steady_clock::duration::max()};
    bench::cpu_time least;
    for (int r = 0; r < repeats; ++r)
    {
        measured   m;
        const auto at    = allocations.load();
        const auto cpu   = bench::cpu_time::now();
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < l.requests; ++i)
        {
            if (co_await get_all(*client, url) != l.size) m.failed = true;
            if (m.failed) break;
        }
        m.took        = std::chrono::steady_clock::now() - start;
        m.cpu         = bench::cpu_time::now() - cpu;
        m.allocations = allocations.load() - at;
        if (m.failed) co_return m;
        if (m.took < best.took) best = m;
        least = least.least(m.cpu);
    }
    best.cpu = least;
    co_return best;
}

// result is a load's run with a size of reader.
struct result
{
    std::size_t reader = 0;
    load        l;
    measured    m;
};

// run_all runs the loads for each size of the client's reader, a client each.
task<std::vector<result>> run_all(net::io::scheduler* scheduler, std::uint16_t port)
{
    std::vector<result> out;
    for (const auto size : reader_sizes)
    {
        http::client_options options;
        options.http2            = false;
        options.read_buffer_size = size;
        http::client client{scheduler, options};

        for (const auto& l : loads)
        {
            const auto m = co_await run(&client, std::format("http://127.0.0.1:{}{}", port, l.path), l);
            out.push_back(result{.reader = size, .l = l, .m = m});
        }
    }
    co_return out;
}

// measure_http11 runs the loads over HTTP/1.1 for each size of the client's reader.
int measure_http11()
{
    auto               workers = std::make_shared<net::coro::thread_pool>(2);
    net::io::scheduler scheduler{workers};

    http::router router;
    router.GET("/plaintext", plaintext);
    router.GET("/cookies", with_cookies);
    router.GET("/download", download);

    http::server server{
        &scheduler,
        std::move(router),
        {.host = "127.0.0.1", .port = "0"}
    };
    scheduler.spawn(server.serve());

    // (one scheduler, so all of the runs are one task.)
    const auto results = scheduler.run_to_completion(run_all(&scheduler, server.local_port()));

    std::println("cppnet's HTTP/1.1 client and server, over loopback ({} workers; best of {}; the server's allocations "
                 "too):",
                 workers->concurrency(),
                 repeats);

    int status = 0;
    for (const auto& [size, l, m] : results)
    {
        if (m.failed)
        {
            std::println(stderr, "{}, a {} B reader: a request failed", l.name, size);
            status = 1;
            continue;
        }

        const auto per = std::chrono::duration<double, std::micro>(m.took).count() / l.requests;
        std::println("a {:>5} B reader, {}: {:.2f} us per request ({:.1f} MiB/s), {}, {:.2f} allocations per request",
                     size,
                     l.name,
                     per,
                     static_cast<double>(l.size) / per * 1e6 / (1'024.0 * 1'024.0),
                     m.cpu.per(l.requests),
                     static_cast<double>(m.allocations) / l.requests);
    }

    server.close();
    scheduler.shutdown();
    return status;
}

// stream_outcome is how long a download and an upload took, or that one failed.
struct stream_outcome
{
    std::chrono::steady_clock::duration down{};
    std::chrono::steady_clock::duration up{};
    bool                                failed = false;
};

// transfer downloads size bytes from base's /download, then uploads as many to its /upload (after a request to open the
// connection, so neither counts its handshake).
task<stream_outcome> transfer(http::client* client, std::string base, std::size_t size)
{
    stream_outcome out;
    if (co_await get_all(*client, base + "/plaintext") != body.size()) co_return stream_outcome{.failed = true};

    auto start = std::chrono::steady_clock::now();
    {
        auto resp = co_await client->get(std::format("{}/bytes/{}", base, size));
        if (!resp.has_value() || resp->version.major != 2) co_return stream_outcome{.failed = true};

        std::vector<std::byte> scratch(std::size_t{64} * 1'024);
        std::size_t            read = 0;
        while (true)
        {
            auto res = co_await resp->body->read(scratch);
            read += res.count;
            if (res.err) break;
        }
        if (read != size) co_return stream_outcome{.failed = true};
    }
    out.down = std::chrono::steady_clock::now() - start;

    static const std::string payload(size, 'u');
    http::client_request req{.method = http::request_method::POST, .uri = net::url::parse(base + "/upload").value()};
    req.headers.set_content_length(size);
    req.body = std::make_unique<net::io::string_reader<char>>(payload);

    start     = std::chrono::steady_clock::now();
    auto resp = co_await client->send(std::move(req));
    if (!resp.has_value() || resp->status_code != http::status::OK) co_return stream_outcome{.failed = true};
    (void)co_await net::io::read_all(*resp->body);
    out.up = std::chrono::steady_clock::now() - start;
    co_return out;
}

// bytes answers with the bytes its path asks for (/bytes/:size), written 64 KiB at a time.
task<void> bytes(const http::server_request& req, http::response_writer& resp)
{
    static const std::string piece(std::size_t{64} * 1'024, 'b');

    const auto size = std::stoul(std::string{req.path_value("size")});

    auto* out = co_await resp.send(http::status::OK, size);
    for (std::size_t sent = 0; sent < size; sent += piece.size())
        co_await net::io::write_all(*out, std::string_view{piece}.substr(0, std::min(piece.size(), size - sent)));
}

// measure_stream runs a download and an upload over one HTTP/2 stream, with the windows given.
int measure_stream(std::uint32_t client_window, std::uint32_t server_window, std::size_t mib)
{
    auto               workers = std::make_shared<net::coro::thread_pool>(2);
    net::io::scheduler scheduler{workers};

    http::router router;
    router.GET("/plaintext", plaintext);
    router.GET("/bytes/:size", bytes);
    router.POST("/upload", upload);

    http::server server{
        &scheduler,
        std::move(router),
        {.host = "127.0.0.1", .port = "0", .http2_stream_window = server_window}
    };
    scheduler.spawn(server.serve());

    http::client_options options;
    options.http2_prior_knowledge = true;
    options.http2_stream_window   = client_window;
    http::client client{&scheduler, options};

    const auto size = mib << 20;
    const auto out =
        scheduler.run_to_completion(transfer(&client, std::format("http://127.0.0.1:{}", server.local_port()), size));

    int status = 0;
    if (out.failed)
    {
        std::println(stderr, "a transfer failed");
        status = 1;
    }
    else
    {
        const auto rate = [&](std::chrono::steady_clock::duration took)
        { return static_cast<double>(mib) / std::chrono::duration<double>(took).count(); };
        std::println("one HTTP/2 stream, {} MiB: the client's window {} B, down {:.2f} s ({:.1f} MiB/s); the server's "
                     "window {} B, up {:.2f} s ({:.1f} MiB/s)",
                     mib,
                     client_window,
                     std::chrono::duration<double>(out.down).count(),
                     rate(out.down),
                     server_window,
                     std::chrono::duration<double>(out.up).count(),
                     rate(out.up));
    }

    server.close();
    scheduler.shutdown();
    return status;
}

}

int main(int argc, char** argv)
{
    spdlog::set_level(spdlog::level::warn);

    const std::span<char*> args{argv, static_cast<std::size_t>(argc)};
    if (args.size() > 1 && std::string_view{args[1]} == "stream")
    {
        if (args.size() < 4)
        {
            std::println(stderr, "usage: bench_client stream <client's window> <server's window> [MiB]");
            return 2;
        }
        const auto mib = args.size() > 4 ? std::stoul(args[4]) : 64UL;
        return measure_stream(static_cast<std::uint32_t>(std::stoul(args[2])),
                              static_cast<std::uint32_t>(std::stoul(args[3])),
                              mib);
    }
    return measure_http11();
}
