#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "coro/task.hpp"
#include "dns/config.hpp"
#include "dns/message.hpp"
#include "io/buffered_reader.hpp"
#include "io/io.hpp"
#include "io/reader.hpp"
#include "io/scheduler.hpp"
#include "io/writer.hpp"
#include "ip_addr.hpp"
#include "ipv4_addr.hpp"
#include "ipv6_addr.hpp"
#include "listen.hpp"
#include "tcp.hpp"
#include "udp.hpp"
#include "util/string_util.hpp"

// A fake nameserver, for the native resolver's tests: a bound UDP socket and a TCP listener on the same port, on
// loopback, answering each query as a test's handler says.
namespace test
{

// fake_query is a query the fake server received.
struct fake_query
{
    net::dns::message          msg{};
    bool                       tcp  = false;
    std::uint16_t              port = 0; // the port it came from (UDP's: the resolver's socket's)
    net::io::clock::time_point when{};
};

// fake_reply is a message the fake server sends back to a query, after a delay. (A reply's delay holds up the queries
// that come after it, on UDP, which are read one at a time.)
struct fake_reply
{
    net::dns::message         msg{};
    std::chrono::milliseconds delay{};
};

// fake_handler is what the fake server sends back to a query: any number of replies, none to drop it. It's called on
// the scheduler's threads, one call at a time.
using fake_handler = std::function<std::vector<fake_reply>(const fake_query&)>;

// reply_to is a response to query: its ID, question and EDNS(0), recursion available, with answers and rcode.
[[nodiscard]] inline net::dns::message reply_to(const net::dns::message&      query,
                                                std::vector<net::dns::record> answers = {},
                                                net::dns::rcode               rcode   = net::dns::rcode::no_error)
{
    net::dns::message msg;
    msg.header.id                  = query.header.id;
    msg.header.response            = true;
    msg.header.recursion_desired   = query.header.recursion_desired;
    msg.header.recursion_available = true;
    msg.header.rcode               = rcode;
    msg.questions                  = query.questions;
    msg.answers                    = std::move(answers);
    msg.edns                       = query.edns;
    return msg;
}

[[nodiscard]] inline net::dns::record a(std::string name, net::ipv4_addr addr)
{
    return {.name = std::move(name), .rclass = net::dns::rclass::in, .ttl = 60, .data = addr};
}

[[nodiscard]] inline net::dns::record aaaa(std::string name, net::ipv6_addr addr)
{
    return {.name = std::move(name), .rclass = net::dns::rclass::in, .ttl = 60, .data = addr};
}

[[nodiscard]] inline net::dns::record cname(std::string name, std::string target)
{
    return {.name   = std::move(name),
            .rclass = net::dns::rclass::in,
            .ttl    = 60,
            .data   = net::dns::cname{.target = std::move(target)}};
}

// zone is a fake server's records, by name (lower-cased).
using zone = std::map<std::string, std::vector<net::dns::record>, std::less<>>;

// answer_from answers queries as a recursive server would from z: a name's records of the type asked for, or its CNAME
// and then its target's (up to 16 links, so a loop is sent as far as that), or NXDOMAIN for a name z hasn't.
[[nodiscard]] inline fake_handler answer_from(zone z)
{
    return [z = std::move(z)](const fake_query& q) -> std::vector<fake_reply>
    {
        if (q.msg.questions.size() != 1) return {};
        const auto& question = q.msg.questions.front();

        std::string name = question.name;
        for (auto& c : name) c = net::util::to_lower_ascii(c);

        std::vector<net::dns::record> answers;
        for (int links = 0; links <= 16; ++links)
        {
            const auto it = z.find(name);
            if (it == z.end())
            {
                if (links == 0) return {{.msg = reply_to(q.msg, {}, net::dns::rcode::name_error)}};
                break;
            }

            std::optional<std::string> alias;
            for (const auto& r : it->second)
            {
                if (r.type() == question.type)
                {
                    answers.push_back(r);
                }
                else if (const auto* c = std::get_if<net::dns::cname>(&r.data); c != nullptr)
                {
                    answers.push_back(r);
                    alias = c->target;
                }
            }

            if (!alias.has_value() || question.type == net::dns::type::cname) break;
            name = *alias;
        }

        return {{.msg = reply_to(q.msg, std::move(answers))}};
    };
}

// fake_server serves on addr (loopback by default), on a port of the OS's choosing, from when it's made until it's
// destroyed (or its scheduler stops). It must be destroyed before its scheduler, and what its handler uses must outlive
// it: declared after the scheduler, and after what the handler uses.
class fake_server
{
public:
    fake_server(net::io::scheduler* sched, fake_handler handler, const net::ip_addr& addr = net::ipv4_addr::loopback())
        : st{std::make_shared<state>(sched, std::move(handler))}
    {
        // a TCP listener on a port the OS chooses, then a UDP socket on the same one: which another program may hold,
        // so it tries a few. (Nothing better: the OS can't be asked for a port free for both.)
        for (int tries = 0;; ++tries)
        {
            auto tcp = net::listener::make(sched, addr.to_string(), "0");
            REQUIRE(tcp.has_value());

            auto udp = net::udp_socket::bind(sched, addr, tcp->local_port());
            if (udp.has_value())
            {
                st->port = tcp->local_port();
                st->tcp.emplace(std::move(*tcp));
                st->udp.emplace(std::move(*udp));
                break;
            }

            REQUIRE(udp.error() == std::errc::address_in_use);
            REQUIRE(tries < 10);
        }

        st->addr = addr;
        st->udp->set_stop_token(st->stop.get_token());

        REQUIRE(sched->spawn(serve_udp(st)));
        REQUIRE(sched->spawn(serve_tcp(st)));
    }

    fake_server(const fake_server&)            = delete;
    fake_server& operator=(const fake_server&) = delete;
    fake_server(fake_server&&)                 = delete;
    fake_server& operator=(fake_server&&)      = delete;

    ~fake_server()
    {
        st->stop.request_stop();
        if (st->tcp.has_value()) st->tcp->shutdown();
    }

    [[nodiscard]] std::uint16_t port() const noexcept { return st->port; }

    [[nodiscard]] net::dns::nameserver nameserver() const noexcept { return {.addr = st->addr, .port = st->port}; }

    // queries is what it has received, in order.
    [[nodiscard]] std::vector<fake_query> queries() const
    {
        const std::scoped_lock lock{st->mu};
        return st->log;
    }

    // received counts the queries it has received: notified (with the scheduler's notify_all()) as it does.
    [[nodiscard]] const std::atomic<std::uint64_t>& received() const noexcept { return st->received; }

private:
    struct state
    {
        state(net::io::scheduler* sched, fake_handler handler)
            : sched{sched}
            , handler{std::move(handler)}
        {}

        net::io::scheduler* sched;
        fake_handler        handler;

        mutable std::mutex         mu; // the log, and calls of the handler
        std::vector<fake_query>    log;
        std::atomic<std::uint64_t> received{0};

        std::stop_source               stop;
        net::ip_addr                   addr;
        std::uint16_t                  port = 0;
        std::optional<net::udp_socket> udp;
        std::optional<net::listener>   tcp;

        // take logs q, and returns the handler's replies.
        std::vector<fake_reply> take(fake_query q)
        {
            std::vector<fake_reply> replies;
            {
                const std::scoped_lock lock{mu};
                replies = handler(q);
                log.push_back(std::move(q));
                received.fetch_add(1);
            }
            sched->notify_all(received);
            return replies;
        }
    };

    static net::coro::task<std::error_code> read_exact(net::io::reader& in, std::span<std::byte> data)
    {
        while (!data.empty())
        {
            auto res = co_await in.read(data);
            if (res.err) co_return res.err;
            data = data.subspan(res.count);
        }
        co_return std::error_code{};
    }

    static net::coro::task<> serve_udp(std::shared_ptr<state> st)
    {
        std::vector<std::byte> buf(65'535);
        std::vector<std::byte> out;

        while (true)
        {
            auto got = co_await st->udp->receive_from(buf);
            if (!got.has_value()) co_return;

            auto msg = net::dns::decode(std::span{buf}.first(got->size));
            if (!msg.has_value()) continue;

            auto replies =
                st->take({.msg = std::move(*msg), .tcp = false, .port = got->port, .when = net::io::clock::now()});
            for (const auto& reply : replies)
            {
                if (reply.delay > std::chrono::milliseconds::zero())
                {
                    auto err =
                        co_await st->sched->sleep_until(net::io::clock::now() + reply.delay, st->stop.get_token());
                    if (err) co_return;
                }

                out.clear();
                if (net::dns::encode(reply.msg, out)) continue;
                (void)co_await st->udp->send_to(out, got->addr, got->port);
            }
        }
    }

    static net::coro::task<> serve_tcp(std::shared_ptr<state> st)
    {
        while (true)
        {
            auto conn = co_await st->tcp->accept();
            if (!conn.has_value()) co_return;
            if (!st->sched->spawn(serve_connection(st, std::move(*conn)))) co_return;
        }
    }

    static net::coro::task<> serve_connection(std::shared_ptr<state> st, net::tcp_socket conn)
    {
        conn.set_stop_token(st->stop.get_token());

        net::io::buffered_reader in{&conn};
        std::vector<std::byte>   body;
        std::vector<std::byte>   out;

        while (true)
        {
            std::array<std::byte, 2> length{};
            if (auto err = co_await read_exact(in, length); err) co_return;

            body.resize((std::to_integer<std::size_t>(length[0]) << 8U) | std::to_integer<std::size_t>(length[1]));
            if (auto err = co_await read_exact(in, body); err) co_return;

            auto msg = net::dns::decode(body);
            if (!msg.has_value()) co_return;

            auto replies = st->take({.msg = std::move(*msg), .tcp = true, .port = 0, .when = net::io::clock::now()});
            for (const auto& reply : replies)
            {
                if (reply.delay > std::chrono::milliseconds::zero())
                {
                    auto err =
                        co_await st->sched->sleep_until(net::io::clock::now() + reply.delay, st->stop.get_token());
                    if (err) co_return;
                }

                out.assign(2, std::byte{0});
                if (net::dns::encode(reply.msg, out)) continue;

                const auto size = out.size() - 2;
                out[0]          = static_cast<std::byte>(size >> 8U);
                out[1]          = static_cast<std::byte>(size & 0xffU);

                auto res = co_await net::io::write_all(conn, std::span<const std::byte>{out});
                if (res.err) co_return;
            }
        }
    }

    std::shared_ptr<state> st;
};

}
