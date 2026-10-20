#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_tostring.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "coro/task.hpp"
#include "coro/thread_pool.hpp"
#include "http/client.hpp"
#include "http/connection_pool.hpp"
#include "http/headers.hpp"
#include "http/http.hpp"
#include "http/request.hpp"
#include "http/request_target.hpp"
#include "http/response.hpp"
#include "http/server.hpp"
#include "io/io.hpp"
#include "io/reader.hpp"
#include "io/scheduler.hpp"
#include "io/string_reader.hpp"
#include "io/util.hpp"
#include "io/writer.hpp"
#include "listen.hpp"
#include "url.hpp"

namespace
{

using namespace std::string_view_literals;

using net::coro::task;
using net::http::request_method;
using net::http::status;

// reply is what a scripted server answers a path with.
struct reply
{
    status      code = status::OK;
    std::string location{}; // none, if empty
    std::string body{};
    bool        chunked = false; // the body sent without a Content-Length
    bool        stop    = false; // stop the test's request, and send nothing
};

// seen is a request a scripted server got.
struct seen
{
    request_method method = request_method::NONE;
    std::string    target{};  // the path, and the query (after a '?') if there's one
    std::string    host{};    // server_request::host: the Host field, or HTTP/2's :authority
    std::string    body{};    // as read
    std::uint32_t  version{}; // the major version

    std::optional<std::string> authorization{};
    std::optional<std::string> proxy_authorization{};
    std::optional<std::string> cookie{};
    std::optional<std::string> custom{}; // X-Custom, which goes everywhere
    std::optional<std::string> content_type{};
    std::optional<std::string> content_length{};
    std::optional<std::string> content_language{};
};

std::optional<std::string> field(const net::http::headers& headers, std::string_view name)
{
    auto value = headers.get(name);
    return value.has_value() ? std::optional<std::string>{*value} : std::nullopt;
}

// scripted is a server on 127.0.0.1 answering each path as its replies say (404 for others), and recording what it
// gets. Its replies are set before it serves.
class scripted
{
public:
    explicit scripted(net::io::scheduler* sched)
        : server{sched,
                 net::listener::make(sched, "127.0.0.1", "0").value(),
                 [this](net::http::server_request& req, net::http::response_writer& resp) { return handle(req, resp); }}
    {}

    [[nodiscard]] std::string port() const { return std::to_string(server.local_port()); }
    [[nodiscard]] std::string url(std::string_view target) const
    {
        return std::format("http://127.0.0.1:{}{}", server.local_port(), target);
    }

    [[nodiscard]] std::vector<seen> requests() const
    {
        std::scoped_lock lock{mu};
        return got;
    }

    task<> serve() { return server.serve(); }

    std::map<std::string, reply, std::less<>> replies;
    std::stop_source*                         stop = nullptr; // for reply::stop

private:
    task<> handle(net::http::server_request& req, net::http::response_writer& resp)
    {
        std::string body;
        if (req.body != nullptr)
        {
            auto read = co_await net::io::read_all(*req.body);
            body      = read.has_value() ? *read : "[" + read.error().message() + "]";
        }

        {
            std::scoped_lock lock{mu};
            got.push_back(seen{
                .method        = req.method,
                .target        = req.target.query.empty() ? req.target.path : req.target.path + "?" + req.target.query,
                .host          = req.host,
                .body          = std::move(body),
                .version       = req.version.major,
                .authorization = field(req.headers, "Authorization"sv),
                .proxy_authorization = field(req.headers, "Proxy-Authorization"sv),
                .cookie              = field(req.headers, "Cookie"sv),
                .custom              = field(req.headers, "X-Custom"sv),
                .content_type        = field(req.headers, "Content-Type"sv),
                .content_length      = field(req.headers, "Content-Length"sv),
                .content_language    = field(req.headers, "Content-Language"sv),
            });
        }

        // by the target, or else by the path.
        auto found = replies.find(req.target.path + "?" + req.target.query);
        if (found == replies.end()) found = replies.find(req.target.path);
        if (found == replies.end())
        {
            (void)co_await resp.send(status::NOT_FOUND, 0);
            co_return;
        }

        const auto& r = found->second;
        if (r.stop)
        {
            stop->request_stop();
            co_return;
        }

        if (!r.location.empty()) resp.headers().set("Location", r.location);
        auto* out = r.chunked ? co_await resp.send(r.code) : co_await resp.send(r.code, r.body.size());
        if (!r.body.empty()) (void)co_await out->write(r.body);
    }

    mutable std::mutex mu;
    std::vector<seen>  got;

    // NOTE: last, so it's destroyed first, while what its handlers use is still there.
    net::http::server server;
};

net::http::client_request make_request(request_method method, const std::string& url)
{
    return net::http::client_request{.method = method, .uri = net::url::parse(url).value()};
}

// outcome is what a request came to: its response's status, URL and body, or why it failed; and the client's
// connections once it was done with (altogether, and to each of the ports asked for, on 127.0.0.1).
struct outcome
{
    status                                   code = status::NONE;
    std::string                              url{};
    std::string                              body{};
    std::error_code                          err{};
    net::http::connection_stats              stats{};
    std::vector<net::http::connection_stats> to{};
};

task<outcome> exchange(net::http::client* client, net::http::client_request req)
{
    auto resp = co_await client->send(std::move(req));
    if (!resp.has_value()) co_return outcome{.err = resp.error()};

    auto body = co_await net::io::read_all(*resp->body);
    co_return outcome{
        .code = resp->status_code,
        .url  = resp->url.build(),
        .body = body.has_value() ? *body : "[" + body.error().message() + "]",
    };
}

// NOTE: the stats while the scheduler runs, as stopping it ends HTTP/2's connections.
task<outcome> send(net::http::client* client, net::http::client_request req, std::vector<std::string> ports = {})
{
    auto out  = co_await exchange(client, std::move(req));
    out.stats = client->stats();
    for (const auto& port : ports) out.to.push_back(client->stats("127.0.0.1", port));
    co_return out;
}

net::http::client_options following(std::size_t max_redirects, bool http2 = false)
{
    net::http::client_options options;
    options.max_redirects         = max_redirects;
    options.http2_prior_knowledge = http2;
    return options;
}

}

template<>
struct Catch::StringMaker<net::http::connection_stats>
{
    static std::string convert(const net::http::connection_stats& stats)
    {
        return std::format("{{open {}, idle {}}}", stats.open, stats.idle);
    }
};

TEST_CASE("redirects are followed only when asked to", "[http][client][redirect]")
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(2)};
    scripted           server{&sched};
    server.replies["/start"] = reply{.code = status::FOUND, .location = "/done", .body = "moved"};
    server.replies["/done"]  = reply{.body = "done"};
    REQUIRE(sched.spawn(server.serve()));

    const auto start = server.url("/start");

    SECTION("by default, a redirect's response is returned")
    {
        net::http::client client{&sched};
        const auto        got = sched.run_to_completion(send(&client, make_request(request_method::GET, start)));
        CHECK(got.code == status::FOUND);
        CHECK(got.body == "moved");
        CHECK(got.url == start);
        CHECK(server.requests().size() == 1);
    }

    SECTION("the client's limit")
    {
        net::http::client client{&sched, following(1)};
        const auto        got = sched.run_to_completion(send(&client, make_request(request_method::GET, start)));
        CHECK(got.code == status::OK);
        CHECK(got.body == "done");
        CHECK(got.url == server.url("/done"));
        CHECK(server.requests().size() == 2);
    }

    SECTION("a request's limit overrides the client's")
    {
        const auto client_limit  = GENERATE(std::size_t{0}, std::size_t{10});
        const auto request_limit = client_limit == 0 ? std::size_t{10} : std::size_t{0};
        CAPTURE(client_limit, request_limit);

        net::http::client client{&sched, following(client_limit)};
        auto              req = make_request(request_method::GET, start);
        req.max_redirects     = request_limit;

        const auto got = sched.run_to_completion(send(&client, std::move(req)));
        CHECK(got.code == (request_limit == 0 ? status::FOUND : status::OK));
    }
}

TEST_CASE("a response says which URL it answers", "[http][client][redirect]")
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(2)};
    scripted           server{&sched};
    server.replies["/page"] = reply{.body = "page"};
    REQUIRE(sched.spawn(server.serve()));

    // whether it was redirected or not, the fragment and all.
    net::http::client client{&sched, following(10)};
    const auto        url = server.url("/page?q=1#part");
    const auto        got = sched.run_to_completion(send(&client, make_request(request_method::GET, url)));
    CHECK(got.code == status::OK);
    CHECK(got.url == url);
}

TEST_CASE("redirects change the method and drop the body as their status says", "[http][client][redirect]")
{
    struct test_case
    {
        status         code;
        request_method method;
        request_method next;
        bool           body       = false; // the request has one, and so does the next (if keeps_body)
        bool           keeps_body = false;
    };

    using enum request_method;

    const auto tc = GENERATE(
        test_case{.code = status::MOVED_PERMANENTLY, .method = POST, .next = GET, .body = true},
        test_case{.code = status::FOUND, .method = PUT, .next = GET, .body = true},
        test_case{.code = status::FOUND, .method = DELETE, .next = GET},
        test_case{.code = status::SEE_OTHER, .method = POST, .next = GET, .body = true},
        test_case{.code = status::SEE_OTHER, .method = PATCH, .next = GET, .body = true},
        test_case{.code = status::MOVED_PERMANENTLY, .method = GET, .next = GET},
        test_case{.code = status::FOUND, .method = HEAD, .next = HEAD},
        test_case{.code = status::SEE_OTHER, .method = HEAD, .next = HEAD},
        test_case{.code = status::SEE_OTHER, .method = GET, .next = GET},
        test_case{.code = status::TEMPORARY_REDIRECT, .method = POST, .next = POST, .body = true, .keeps_body = true},
        test_case{.code = status::PERMANENT_REDIRECT, .method = PUT, .next = PUT, .body = true, .keeps_body = true},
        test_case{.code = status::TEMPORARY_REDIRECT, .method = DELETE, .next = DELETE},
        test_case{.code = status::PERMANENT_REDIRECT, .method = GET, .next = GET});
    const bool http2 = GENERATE(false, true);
    CAPTURE(static_cast<int>(tc.code), net::http::method_string(tc.method), http2);

    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(2)};
    scripted           server{&sched};
    server.replies["/start"] = reply{.code = tc.code, .location = "/next"};
    server.replies["/next"]  = reply{.body = "done"};
    REQUIRE(sched.spawn(server.serve()));

    net::http::client client{&sched, following(10, http2)};

    auto req = make_request(tc.method, server.url("/start"));
    req.headers.set("X-Custom", "kept");
    if (tc.body)
    {
        req.set_body("payload");
        req.headers.set("Content-Type", "text/plain");
        req.headers.set("Content-Language", "en");
    }

    const auto got = sched.run_to_completion(send(&client, std::move(req)));
    REQUIRE_FALSE(got.err);
    CHECK(got.code == status::OK);
    CHECK(got.body == (tc.next == HEAD ? "" : "done"));

    const auto requests = server.requests();
    REQUIRE(requests.size() == 2);
    CHECK(requests[0].body == (tc.body ? "payload" : ""));

    const auto& next = requests[1];
    CHECK(next.version == (http2 ? 2U : 1U));
    CHECK(next.method == tc.next);
    CHECK(next.target == "/next");
    CHECK(next.custom == "kept");

    const bool has_body = tc.body && tc.keeps_body;
    CHECK(next.body == (has_body ? "payload" : ""));
    CHECK(next.content_type == (has_body ? std::optional<std::string>{"text/plain"} : std::nullopt));
    CHECK(next.content_language == (has_body ? std::optional<std::string>{"en"} : std::nullopt));
    if (has_body) CHECK(next.content_length == "7");
    else if (!http2) CHECK(next.content_length == std::nullopt);
}

TEST_CASE("a 307 or 308 isn't followed without the body again", "[http][client][redirect]")
{
    const auto code = GENERATE(status::TEMPORARY_REDIRECT, status::PERMANENT_REDIRECT);
    CAPTURE(static_cast<int>(code));

    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(2)};
    scripted           server{&sched};
    server.replies["/start"] = reply{.code = code, .location = "/next", .body = "again"};
    server.replies["/next"]  = reply{.body = "done"};
    REQUIRE(sched.spawn(server.serve()));

    net::http::client client{&sched, following(10)};

    auto req = make_request(request_method::POST, server.url("/start"));
    req.headers.set_content_length(7);
    req.body = std::make_unique<net::io::owned_string_reader>("payload");

    SECTION("a streamed body, without get_body")
    {}

    SECTION("get_body returns null")
    {
        req.get_body = [] { return std::unique_ptr<net::io::reader>{}; };
    }

    const auto got = sched.run_to_completion(send(&client, std::move(req)));
    CHECK(got.code == code);
    CHECK(got.body == "again");
    CHECK(got.url == server.url("/start"));
    CHECK(server.requests().size() == 1);
}

TEST_CASE("a redirect's Location is resolved against the request's URL", "[http][client][redirect]")
{
    struct test_case
    {
        std::string_view start;    // the request's path, query and fragment
        std::string_view location; // after the server's origin, if it starts with '@'
        std::string_view target;   // what the next request's for
        std::string_view url;      // the response's, after the server's origin
    };

    const auto tc =
        GENERATE(test_case{.start = "/a/x/y", .location = "../b/c?x=1", .target = "/a/b/c?x=1", .url = "/a/b/c?x=1"},
                 test_case{.start = "/a/x/y", .location = "z", .target = "/a/x/z", .url = "/a/x/z"},
                 test_case{.start = "/a/x/y?q=1", .location = "?q=2", .target = "/a/x/y?q=2", .url = "/a/x/y?q=2"},
                 test_case{.start = "/a/x/y", .location = "/./b/../c/", .target = "/c/", .url = "/c/"},
                 // the fragment kept when Location has none, and replaced when it has one.
                 test_case{.start = "/a#frag", .location = "/b", .target = "/b", .url = "/b#frag"},
                 test_case{.start = "/a#frag", .location = "/b#new", .target = "/b", .url = "/b#new"},
                 // an absolute one, as a signed URL's, arrives as written: encodings, '+', the parameters' order.
                 test_case{.start    = "/a",
                           .location = "@/files/a%2Fb%2fc/x+y?Signature=ab+c/d%3D&Expires=1&a=%2F%2f",
                           .target   = "/files/a%2Fb%2fc/x+y?Signature=ab+c/d%3D&Expires=1&a=%2F%2f",
                           .url      = "/files/a%2Fb%2fc/x+y?Signature=ab+c/d%3D&Expires=1&a=%2F%2f"});
    const bool http2 = GENERATE(false, true);
    CAPTURE(tc.start, tc.location, http2);

    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(2)};
    scripted           server{&sched};

    server.replies[std::string{tc.start.substr(0, tc.start.find('#'))}] =
        reply{.code     = status::FOUND,
              .location = tc.location.starts_with('@') ? server.url(tc.location.substr(1)) : std::string{tc.location}};

    server.replies[std::string{tc.target}] = reply{.body = "done"};
    REQUIRE(sched.spawn(server.serve()));

    net::http::client client{&sched, following(10, http2)};

    const auto got = sched.run_to_completion(send(&client, make_request(request_method::GET, server.url(tc.start))));
    CHECK(got.code == status::OK);
    CHECK(got.url == server.url(tc.url));

    const auto requests = server.requests();
    REQUIRE(requests.size() == 2);
    CHECK(requests[1].target == tc.target);
}

TEST_CASE("redirects are followed up to the limit", "[http][client][redirect]")
{
    const auto hops  = GENERATE(std::size_t{1}, std::size_t{10}, std::size_t{11});
    const bool http2 = GENERATE(false, true);
    CAPTURE(hops, http2);

    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(2)};
    scripted           server{&sched};
    for (std::size_t i = 0; i < hops; ++i)
        server.replies[std::format("/hop/{}", i)] =
            reply{.code = status::FOUND, .location = std::format("/hop/{}", i + 1), .body = "moved"};
    server.replies[std::format("/hop/{}", hops)] = reply{.body = "done"};
    REQUIRE(sched.spawn(server.serve()));

    net::http::client client{&sched, following(10, http2)};

    const auto got = sched.run_to_completion(send(&client, make_request(request_method::GET, server.url("/hop/0"))));

    if (hops <= 10)
    {
        CHECK(got.code == status::OK);
        CHECK(got.body == "done");
        CHECK(got.url == server.url(std::format("/hop/{}", hops)));
        CHECK(server.requests().size() == hops + 1);
    }
    else
    {
        // the eleventh redirect isn't followed.
        CHECK(got.err == net::http::client_errc::too_many_redirects);
        CHECK(server.requests().size() == 11);
    }

    // each redirect's response was read, so its connection was reused; but not the one that wasn't followed (unless
    // it's HTTP/2's, whose stream was reset).
    const bool kept = hops <= 10 || http2;
    CHECK(got.stats == (kept ? net::http::connection_stats{.open = 1, .idle = 1} : net::http::connection_stats{}));
}

TEST_CASE("redirects that can't be followed fail, or are returned", "[http][client][redirect]")
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(2)};
    scripted           server{&sched};
    net::http::client  client{&sched, following(10)};

    SECTION("a Location that isn't a valid reference")
    {
        server.replies["/start"] = reply{.code = status::MOVED_PERMANENTLY, .location = "http://h:x/"};
        REQUIRE(sched.spawn(server.serve()));

        const auto got =
            sched.run_to_completion(send(&client, make_request(request_method::GET, server.url("/start"))));
        CHECK(got.err == net::http::client_errc::bad_redirect);
        CHECK(got.err.message() == "a redirect's Location isn't a valid URI reference");
    }

    SECTION("a Location that isn't http or https")
    {
        server.replies["/start"] = reply{.code = status::FOUND, .location = "ftp://127.0.0.1/file"};
        REQUIRE(sched.spawn(server.serve()));

        const auto got =
            sched.run_to_completion(send(&client, make_request(request_method::GET, server.url("/start"))));
        CHECK(got.err == std::errc::protocol_not_supported);
    }

    SECTION("a redirect without a Location")
    {
        const auto code = GENERATE(status::MOVED_PERMANENTLY, status::FOUND, status::SEE_OTHER);
        CAPTURE(static_cast<int>(code));

        server.replies["/start"] = reply{.code = code, .body = "nowhere"};
        REQUIRE(sched.spawn(server.serve()));

        const auto got =
            sched.run_to_completion(send(&client, make_request(request_method::GET, server.url("/start"))));
        CHECK(got.code == code);
        CHECK(got.body == "nowhere");
    }

    SECTION("other 3xx statuses")
    {
        const auto code = GENERATE(status::MULTIPLE_CHOICES, status::NOT_MODIFIED, status::USE_PROXY);
        CAPTURE(static_cast<int>(code));

        server.replies["/start"] = reply{.code = code, .location = "/next"};
        server.replies["/next"]  = reply{.body = "done"};
        REQUIRE(sched.spawn(server.serve()));

        const auto got =
            sched.run_to_completion(send(&client, make_request(request_method::GET, server.url("/start"))));
        CHECK(got.code == code);
        CHECK(server.requests().size() == 1);
    }
}

TEST_CASE("credentials only follow redirects to the same server", "[http][client][redirect]")
{
    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(2)};
    scripted           first{&sched};
    scripted           second{&sched};

    // to the same server, then another (another port), then back.
    first.replies["/start"]  = reply{.code = status::FOUND, .location = "/same"};
    first.replies["/same"]   = reply{.code = status::TEMPORARY_REDIRECT, .location = second.url("/other")};
    second.replies["/other"] = reply{.code = status::MOVED_PERMANENTLY, .location = first.url("/back")};
    first.replies["/back"]   = reply{.body = "done"};
    REQUIRE(sched.spawn(first.serve()));
    REQUIRE(sched.spawn(second.serve()));

    net::http::client client{&sched, following(10)};

    auto req = make_request(request_method::GET, first.url("/start"));
    req.headers.set("Authorization", "Bearer secret");
    req.headers.set("Proxy-Authorization", "Basic c2VjcmV0");
    req.headers.set("Cookie", "session=secret");
    req.headers.set("Host", "custom.example");
    req.headers.set("X-Custom", "kept");

    const auto got = sched.run_to_completion(send(&client, std::move(req)));
    REQUIRE_FALSE(got.err);
    CHECK(got.code == status::OK);
    CHECK(got.url == first.url("/back"));

    const auto at_first = first.requests();
    REQUIRE(at_first.size() == 3);
    for (std::size_t i = 0; i < 2; ++i)
    {
        CAPTURE(at_first[i].target);
        CHECK(at_first[i].authorization == "Bearer secret");
        CHECK(at_first[i].proxy_authorization == "Basic c2VjcmV0");
        CHECK(at_first[i].cookie == "session=secret");
        CHECK(at_first[i].host == "custom.example");
        CHECK(at_first[i].custom == "kept");
    }

    // once dropped, they're gone, though the last request is to the first server again.
    const auto at_second = second.requests();
    REQUIRE(at_second.size() == 1);
    for (const auto& r : {at_second[0], at_first[2]})
    {
        CAPTURE(r.target);
        CHECK(r.authorization == std::nullopt);
        CHECK(r.proxy_authorization == std::nullopt);
        CHECK(r.cookie == std::nullopt);
        CHECK(r.custom == "kept");
    }
    // and so is the Host the request set (server_request::host is without the port).
    CHECK(at_second[0].host == "127.0.0.1");
    CHECK(at_first[2].host == "127.0.0.1");
}

TEST_CASE("credentials are kept to the same scheme, host and port, or an upgrade to https", "[http][client][redirect]")
{
    struct test_case
    {
        std::string_view from;
        std::string_view to;
        bool             keeps;
    };

    const auto tc = GENERATE(test_case{.from = "http://a.example/x", .to = "http://a.example/y", .keeps = true},
                             test_case{.from = "http://a.example/x", .to = "http://A.Example:80/y", .keeps = true},
                             test_case{.from = "https://a.example/", .to = "https://a.example:443/", .keeps = true},
                             test_case{.from = "a.example/x", .to = "http://a.example/y", .keeps = true},
                             test_case{.from = "http://a.example/", .to = "https://a.example/", .keeps = true},
                             test_case{.from = "http://a.example:80/", .to = "https://a.example:443/", .keeps = true},
                             test_case{.from = "http://a.example:8080/", .to = "http://a.example:8080/", .keeps = true},
                             // another host: a subdomain, or a parent domain, is too.
                             test_case{.from = "http://a.example/", .to = "http://b.example/", .keeps = false},
                             test_case{.from = "http://a.example/", .to = "http://sub.a.example/", .keeps = false},
                             test_case{.from = "http://sub.a.example/", .to = "http://a.example/", .keeps = false},
                             // another port.
                             test_case{.from = "http://a.example/", .to = "http://a.example:8080/", .keeps = false},
                             // https down to http.
                             test_case{.from = "https://a.example/", .to = "http://a.example/", .keeps = false},
                             // an upgrade, but not on the default ports.
                             test_case{.from = "http://a.example:8080/", .to = "https://a.example/", .keeps = false},
                             test_case{.from = "http://a.example/", .to = "https://a.example:8443/", .keeps = false});
    CAPTURE(tc.from, tc.to);

    const auto from = net::url::parse(tc.from);
    const auto to   = net::url::parse(tc.to);
    REQUIRE(from.has_value());
    REQUIRE(to.has_value());
    CHECK(net::http::detail::keeps_credentials(*from, *to) == tc.keeps);
}

TEST_CASE("a redirect's response is read if it's short, so its connection is reused", "[http][client][redirect]")
{
    struct test_case
    {
        std::string_view name;
        reply            redirect;
        bool             reused;
    };

    const auto tc = GENERATE(
        test_case{
            .name     = "empty",
            .redirect = {.code = status::FOUND},
            .reused   = true
    },
        test_case{.name = "short", .redirect = {.code = status::FOUND, .body = "moved"}, .reused = true},
        test_case{.name     = "short, chunked",
                  .redirect = {.code = status::FOUND, .body = "moved", .chunked = true},
                  .reused   = true},
        test_case{.name     = "2 KiB",
                  .redirect = {.code = status::FOUND, .body = std::string(2'048, 'x')},
                  .reused   = true},
        test_case{.name     = "2 KiB, chunked",
                  .redirect = {.code = status::FOUND, .body = std::string(2'048, 'x'), .chunked = true},
                  .reused   = true},
        test_case{.name     = "longer",
                  .redirect = {.code = status::FOUND, .body = std::string(2'049, 'x')},
                  .reused   = false},
        test_case{.name     = "longer, chunked",
                  .redirect = {.code = status::FOUND, .body = std::string(2'049, 'x'), .chunked = true},
                  .reused   = false},
        test_case{.name     = "long",
                  .redirect = {.code = status::FOUND, .body = std::string(100'000, 'x')},
                  .reused   = false});
    const bool http2 = GENERATE(false, true);
    CAPTURE(tc.name, http2);

    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(2)};
    scripted           first{&sched};
    scripted           second{&sched};

    auto redirect           = tc.redirect;
    redirect.location       = second.url("/done");
    first.replies["/start"] = redirect;
    second.replies["/done"] = reply{.body = "done"};
    REQUIRE(sched.spawn(first.serve()));
    REQUIRE(sched.spawn(second.serve()));

    net::http::client client{&sched, following(10, http2)};

    const auto got = sched.run_to_completion(
        send(&client, make_request(request_method::GET, first.url("/start")), {first.port(), second.port()}));
    CHECK(got.code == status::OK);
    CHECK(got.body == "done");

    // over HTTP/2, the connection's kept either way (the redirect's stream reset, if it's not read).
    const bool kept = http2 || tc.reused;
    REQUIRE(got.to.size() == 2);
    CHECK(got.to[0] == (kept ? net::http::connection_stats{.open = 1, .idle = 1} : net::http::connection_stats{}));
    CHECK(got.to[1] == net::http::connection_stats{.open = 1, .idle = 1});
}

TEST_CASE("a redirected request is canceled by its stop token", "[http][client][redirect]")
{
    const bool http2 = GENERATE(false, true);
    CAPTURE(http2);

    std::stop_source source;

    net::io::scheduler sched{std::make_shared<net::coro::thread_pool>(2)};
    scripted           server{&sched};
    server.stop              = &source;
    server.replies["/start"] = reply{.code = status::SEE_OTHER, .location = "/stop"};
    server.replies["/stop"]  = reply{.stop = true};
    REQUIRE(sched.spawn(server.serve()));

    net::http::client client{&sched, following(10, http2)};

    // stopped as the second request is handled.
    auto req = make_request(request_method::POST, server.url("/start"));
    req.set_body("payload");
    req.stop = source.get_token();

    const auto got = sched.run_to_completion(send(&client, std::move(req)));
    CHECK(got.err == std::errc::operation_canceled);
    CHECK(server.requests().size() == 2);
}
