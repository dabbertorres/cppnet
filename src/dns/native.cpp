// The resolver's native back end: DNS queries over UDP (and TCP, when an answer's truncated), as Go's resolver sends
// them, and the hosts table.

#include "dns/native.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include "config.hpp" // IWYU pragma: keep (for the platform macros)
#include "coro/task.hpp"
#include "dns/config.hpp"
#include "dns/error.hpp"
#include "dns/message.hpp"
#include "io/buffered_reader.hpp"
#include "io/io.hpp"
#include "io/reader.hpp"
#include "io/scheduler.hpp"
#include "io/writer.hpp"
#include "ip_addr.hpp"
#include "ipv4_addr.hpp"
#include "ipv6_addr.hpp"
#include "tcp.hpp"
#include "udp.hpp"
#include "util/string_util.hpp"

#ifdef NET_IS_LINUX
#    include <cerrno>
#    include <exception>

#    include <sys/random.h>
#else
#    include <cstdlib>
#endif

namespace
{

namespace dns = net::dns;

using lookup_result = std::expected<std::vector<net::ip_addr>, std::error_code>;

constexpr std::size_t max_name  = 253;
constexpr std::size_t max_label = 63;

// fill_random fills out from the OS's randomness.
void fill_random(std::span<std::byte> out) noexcept
{
#ifdef NET_IS_LINUX
    while (!out.empty())
    {
        const auto got = ::getrandom(out.data(), out.size(), 0);
        if (got < 0)
        {
            if (errno == EINTR) continue;
            // NOTE: only kernels older than 3.17 lack it; glibc's arc4random() aborts too, without randomness.
            std::terminate();
        }
        out = out.subspan(static_cast<std::size_t>(got));
    }
#else
    ::arc4random_buf(out.data(), out.size());
#endif
}

// avoid_dns is whether name mustn't be asked of DNS servers: one under .onion (RFC 7686), as Go has it.
bool avoid_dns(std::string_view name) noexcept
{
    constexpr std::string_view onion = ".onion";
    if (name.size() < onion.size()) return false;

    name.remove_prefix(name.size() - onion.size());
    return net::util::equal_ignore_case(name, onion);
}

// sendable is whether name (without a final dot) can be put in a query: labels of 1 to 63 bytes, 253 in all.
bool sendable(std::string_view name) noexcept
{
    if (name.empty() || name.size() > max_name) return false;

    std::size_t label = 0;
    for (const char c : name)
    {
        if (c != '.')
        {
            if (++label > max_label) return false;
        }
        else
        {
            if (label == 0) return false;
            label = 0;
        }
    }
    return label != 0;
}

// question is one of a name's questions (its A or its AAAA records), asked of the servers in turn until one answers.
struct question
{
    dns::type qtype = dns::type::a;

    // whether it's asked with EDNS(0): not after a server answered FORMERR to it.
    bool edns = true;

    // the ID of the query asked last, and whether it's still waiting for its answer on the socket it was sent on.
    std::uint16_t id      = 0;
    bool          waiting = false;

    // whether the answer was truncated, so the query must be asked again over TCP.
    bool truncated = false;

    // done once a server has answered it: with addrs, or with err (name_not_found or no_address). Until then, err is
    // why the last server didn't.
    bool                      done = false;
    std::vector<net::ip_addr> addrs{};
    std::error_code           err{};
};

// exchange is what a name's queries share.
struct exchange
{
    net::io::scheduler* scheduler = nullptr;
    const dns::config*  conf      = nullptr;
    std::string_view    name{};
    std::stop_token     stop{};
};

bool canceled(const std::error_code& err) noexcept { return err == std::errc::operation_canceled; }

// same_name is whether two names are the same, ignoring case (as DNS does, for ASCII).
bool same_name(std::string_view lhs, std::string_view rhs) noexcept { return net::util::equal_ignore_case(lhs, rhs); }

// fail records why a server didn't answer q: err itself if it's the resolver's (e.g. errc::bad_response), and otherwise
// (no answer in time, a refusal, ...) errc::server_failure: nothing usable.
void fail(question& q, const std::error_code& err) noexcept
{
    q.err = err.category() == dns::category() ? err : dns::make_error_code(dns::errc::server_failure);
}

// encode_query appends a query of q to out, with a new ID.
std::error_code encode_query(question& q, const exchange& ex, std::vector<std::byte>& out)
{
    q.id = net::dns::detail::random_id();

    auto msg                  = dns::make_query(q.id, ex.name, q.qtype, q.edns);
    msg.header.authentic_data = ex.conf->trust_ad;
    return dns::encode(msg, out);
}

// answers is whether msg is the answer to q's query: a response to it, with its ID, opcode and question.
bool answers(const dns::message& msg, const question& q, std::string_view name) noexcept
{
    if (!msg.header.response || msg.header.opcode != dns::opcode::query || msg.header.id != q.id) return false;
    if (msg.questions.size() != 1) return false;

    const auto& asked = msg.questions.front();
    return asked.type == q.qtype && asked.rclass == dns::rclass::in && same_name(asked.name, name);
}

// address is a record's address, if it's an A or an AAAA record.
std::optional<net::ip_addr> address(const dns::record& r) noexcept
{
    if (const auto* v4 = std::get_if<net::ipv4_addr>(&r.data); v4 != nullptr) return *v4;
    if (const auto* v6 = std::get_if<net::ipv6_addr>(&r.data); v6 != nullptr) return *v6;
    return std::nullopt;
}

// follow finds name's addresses of type qtype in answers, following CNAME records from name to the name that has them
// (at most max_cname_links): none if the chain ends without any. A chain that loops, or is longer, is bad_response.
std::expected<std::vector<net::ip_addr>, std::error_code>
follow(const std::vector<dns::record>& answers, std::string_view name, dns::type qtype)
{
    std::vector<std::string_view> seen{name};

    for (std::size_t links = 0;; ++links)
    {
        std::vector<net::ip_addr>       addrs;
        std::optional<std::string_view> alias;

        for (const auto& r : answers)
        {
            if (r.rclass != dns::rclass::in || !same_name(r.name, name)) continue;

            if (r.type() == qtype)
            {
                const auto addr = address(r);
                if (addr.has_value() && std::ranges::find(addrs, *addr) == addrs.end()) addrs.push_back(*addr);
            }
            else if (const auto* c = std::get_if<dns::cname>(&r.data); c != nullptr && !alias.has_value())
            {
                alias = c->target;
            }
        }

        if (!addrs.empty() || !alias.has_value()) return addrs;

        const bool looped = std::ranges::any_of(seen, [&](std::string_view s) { return same_name(s, *alias); });
        if (looped || links == net::dns::detail::max_cname_links)
            return std::unexpected(dns::make_error_code(dns::errc::bad_response));

        seen.push_back(*alias);
        name = *alias;
    }
}

// answer takes a server's answer to q (not truncated), which either ends it or says why the server was no use.
void answer(question& q, const dns::message& msg, std::string_view name)
{
    switch (msg.header.rcode)
    {
    case dns::rcode::no_error:
    {
        auto addrs = follow(msg.answers, name, q.qtype);
        if (!addrs.has_value())
        {
            q.err = addrs.error();
            return;
        }

        q.done = true;
        if (addrs->empty())
        {
            q.err = dns::make_error_code(dns::errc::no_address);
        }
        else
        {
            q.addrs = std::move(*addrs);
            q.err.clear();
        }
        return;
    }

    case dns::rcode::name_error:
        q.done = true;
        q.err  = dns::make_error_code(dns::errc::name_not_found);
        return;

    default:
        // SERVFAIL, REFUSED, FORMERR (to a query without EDNS(0)), ...: the next server may do better.
        q.err = dns::rcode_error(msg.header.rcode);
        return;
    }
}

// read_exact reads into data until it's full.
net::coro::task<std::error_code> read_exact(net::io::reader& in, std::span<std::byte> data)
{
    while (!data.empty())
    {
        auto res = co_await in.read(data);
        if (res.err) co_return res.err;
        data = data.subspan(res.count);
    }
    co_return std::error_code{};
}

// send_udp sends a query of q on sock, which then waits for its answer.
net::coro::task<std::error_code> send_udp(net::udp_socket& sock, question& q, const exchange& ex)
{
    std::vector<std::byte> out;
    if (auto err = encode_query(q, ex, out); err) co_return err;

    auto res = co_await sock.write(out);
    if (res.err) co_return res.err;

    q.waiting = true;
    co_return std::error_code{};
}

// exchange_udp asks qs on sock (connected to a server), and reads its datagrams until they're all answered (or
// truncated), or the deadline. Datagrams that answer none of them are read past: forgeries, late answers to an earlier
// attempt, and anything else. It returns the error that ended it early (e.g. std::errc::timed_out, or
// std::errc::connection_refused, when the server's port is closed).
net::coro::task<std::error_code> exchange_udp(net::udp_socket&           sock,
                                              std::span<question* const> qs,
                                              const exchange&            ex,
                                              net::io::clock::time_point deadline)
{
    sock.set_read_deadline(deadline);
    sock.set_write_deadline(deadline);
    sock.set_stop_token(ex.stop);

    // NOTE: waiting from the start, so those a failed send leaves unsent are failed with it.
    for (auto* q : qs) q->waiting = true;
    for (auto* q : qs)
    {
        if (auto err = co_await send_udp(sock, *q, ex); err) co_return err;
    }

    std::vector<std::byte> buf(dns::edns_udp_size);
    while (std::ranges::any_of(qs, &question::waiting))
    {
        auto res = co_await sock.read(buf);
        if (res.err == net::io::errc::end_of_stream) continue; // an empty datagram
        if (res.err) co_return res.err;

        auto msg = dns::decode(std::span{buf}.first(res.count));
        if (!msg.has_value()) continue;

        const auto it =
            std::ranges::find_if(qs, [&](const question* q) { return q->waiting && answers(*msg, *q, ex.name); });
        if (it == qs.end()) continue;

        auto& q   = **it;
        q.waiting = false;

        if (msg->header.truncated)
        {
            q.truncated = true;
        }
        else if (msg->header.rcode == dns::rcode::format_error && q.edns)
        {
            // a server that doesn't know EDNS(0): asked again without it (Decided question 7).
            q.edns = false;
            if (auto err = co_await send_udp(sock, q, ex); err) co_return err;
        }
        else
        {
            answer(q, *msg, ex.name);
        }
    }

    co_return std::error_code{};
}

// exchange_tcp asks q of server over TCP (e.g. after a truncated answer), by the deadline: the query, then its answer,
// each after its length (two bytes). An answer that doesn't match it is bad_response (over TCP, it can't be a
// forgery). It returns the error that ended it early, if any.
net::coro::task<std::error_code>
exchange_tcp(const dns::nameserver& server, question& q, const exchange& ex, net::io::clock::time_point deadline)
{
    const auto now = net::io::clock::now();
    if (now >= deadline) co_return std::make_error_code(std::errc::timed_out);

    net::tcp_socket::connect_options options;
    options.timeout = std::chrono::ceil<std::chrono::milliseconds>(deadline - now);
    options.stop    = ex.stop;

    auto conn = co_await net::tcp_socket::connect(ex.scheduler, std::span{&server.addr, 1}, server.port, options);
    if (!conn.has_value()) co_return conn.error();

    conn->set_deadline(deadline);
    conn->set_stop_token(ex.stop);

    net::io::buffered_reader in{&*conn, 512};
    std::vector<std::byte>   out;
    std::vector<std::byte>   reply;

    // at most twice: again without EDNS(0), if the server answers FORMERR to it.
    while (true)
    {
        out.assign(2, std::byte{0});
        if (auto err = encode_query(q, ex, out); err) co_return err;

        const auto size = out.size() - 2;
        out[0]          = static_cast<std::byte>(size >> 8U);
        out[1]          = static_cast<std::byte>(size & 0xffU);

        auto wrote = co_await net::io::write_all(*conn, std::span<const std::byte>{out});
        if (wrote.err) co_return wrote.err;

        std::array<std::byte, 2> length{};
        if (auto err = co_await read_exact(in, length); err) co_return err;

        reply.resize((std::to_integer<std::size_t>(length[0]) << 8U) | std::to_integer<std::size_t>(length[1]));
        if (auto err = co_await read_exact(in, reply); err) co_return err;

        auto msg = dns::decode(reply);
        if (!msg.has_value() || !answers(*msg, q, ex.name)) co_return dns::make_error_code(dns::errc::bad_response);

        if (msg->header.rcode == dns::rcode::format_error && q.edns)
        {
            q.edns = false;
            continue;
        }

        // NOTE: an answer truncated over TCP too is taken as it is.
        answer(q, *msg, ex.name);
        co_return std::error_code{};
    }
}

// ask_tcp asks q over TCP, recording why if it fails. It returns false if it was canceled.
net::coro::task<bool>
ask_tcp(const dns::nameserver& server, question& q, const exchange& ex, net::io::clock::time_point deadline)
{
    auto err = co_await exchange_tcp(server, q, ex, deadline);
    if (canceled(err)) co_return false;
    if (err) fail(q, err);
    co_return true;
}

// ask_udp asks batch on sock (a new one, connected to server, if it has none), then asks what was truncated again over
// TCP, recording why each that wasn't answered wasn't. It returns false if it was canceled.
net::coro::task<bool> ask_udp(const dns::nameserver&          server,
                              std::optional<net::udp_socket>& sock,
                              std::span<question* const>      batch,
                              const exchange&                 ex)
{
    if (!sock.has_value())
    {
        auto made = net::udp_socket::connect(ex.scheduler, server.addr, server.port);
        if (!made.has_value())
        {
            for (auto* q : batch) fail(*q, made.error());
            co_return true;
        }
        sock.emplace(std::move(*made));
    }

    auto err = co_await exchange_udp(*sock, batch, ex, net::io::clock::now() + ex.conf->timeout);
    if (canceled(err)) co_return false;

    for (auto* q : batch)
    {
        if (q->truncated)
        {
            if (!co_await ask_tcp(server, *q, ex, net::io::clock::now() + ex.conf->timeout)) co_return false;
        }
        else if (q->waiting)
        {
            fail(*q, err);
        }
    }
    co_return true;
}

// ask asks server qs' questions that aren't done yet, in one attempt: on one UDP socket (a new one, so its source
// port is new), both at once, unless the configuration says to ask them in turn (single-request), or on sockets of
// their own (single-request-reopen), or over TCP (use-vc). Each waits for at most the configuration's timeout; a
// truncated answer's question is asked again over TCP, with a timeout of its own. It fails only if it's canceled.
net::coro::task<std::error_code> ask(const dns::nameserver& server, std::span<question> qs, const exchange& ex)
{
    const auto& conf = *ex.conf;

    std::vector<question*> open;
    for (auto& q : qs)
    {
        if (q.done) continue;
        q.waiting   = false;
        q.truncated = false;
        open.push_back(&q);
    }

    const auto canceled_err = std::make_error_code(std::errc::operation_canceled);

    if (conf.use_tcp)
    {
        for (auto* q : open)
        {
            if (!co_await ask_tcp(server, *q, ex, net::io::clock::now() + conf.timeout)) co_return canceled_err;
        }
        co_return std::error_code{};
    }

    if (!conf.single_request && !conf.single_request_reopen)
    {
        std::optional<net::udp_socket> sock;
        if (!open.empty() && !co_await ask_udp(server, sock, open, ex)) co_return canceled_err;
        co_return std::error_code{};
    }

    // in turn, on one socket, or (reopening) one each.
    std::optional<net::udp_socket> sock;
    for (auto* q : open)
    {
        if (conf.single_request_reopen) sock.reset();
        if (!co_await ask_udp(server, sock, std::span{&q, 1}, ex)) co_return canceled_err;
    }

    co_return std::error_code{};
}

// ask_servers asks qs of the servers, in rounds (the configuration's attempts), each starting at first, until each
// has been answered. It fails only if it's canceled.
net::coro::task<std::error_code> ask_servers(std::span<question> qs, const exchange& ex, std::size_t first)
{
    const auto& servers = ex.conf->nameservers;

    for (int attempt = 0; attempt < ex.conf->attempts; ++attempt)
    {
        for (std::size_t i = 0; i < servers.size(); ++i)
        {
            if (std::ranges::all_of(qs, &question::done)) co_return std::error_code{};
            if (ex.stop.stop_requested()) co_return std::make_error_code(std::errc::operation_canceled);

            auto err = co_await ask(servers[(first + i) % servers.size()], qs, ex);
            if (err) co_return err;
        }
    }

    co_return std::error_code{};
}

// query_dns looks host up with DNS: each of its names (the search list's) in turn, until one has addresses.
net::coro::task<lookup_result> query_dns(net::io::scheduler*    scheduler,
                                         const dns::config&     conf,
                                         std::string_view       host,
                                         net::protocol          proto,
                                         std::size_t            first,
                                         const std::stop_token& stop)
{
    std::vector<dns::type> types;
    types.reserve(2);
    if (proto != net::protocol::ipv6) types.push_back(dns::type::a);
    if (proto != net::protocol::ipv4) types.push_back(dns::type::aaaa);

    const auto original = host.ends_with('.') ? host.substr(0, host.size() - 1) : host;

    std::error_code last;
    for (const auto& name : net::dns::detail::name_list(conf, host))
    {
        std::vector<question> qs;
        qs.reserve(types.size());
        for (const auto type : types) qs.push_back({.qtype = type});

        const exchange ex{.scheduler = scheduler, .conf = &conf, .name = name, .stop = stop};
        if (auto err = co_await ask_servers(qs, ex, first); err) co_return std::unexpected(err);

        std::vector<net::ip_addr> addrs;
        for (const auto& q : qs)
        {
            if (!q.addrs.empty())
            {
                addrs.insert(addrs.end(), q.addrs.begin(), q.addrs.end());
                continue;
            }

            // the error for the name as it was given wins, as Go has it (e.g. no_address over a search domain's
            // name_not_found).
            if (!last || name == original) last = q.err ? q.err : dns::make_error_code(dns::errc::server_failure);
        }

        if (!addrs.empty())
        {
            net::dns::detail::sort_by_rfc6724(addrs);
            co_return addrs;
        }
    }

    co_return std::unexpected(last ? last : dns::make_error_code(dns::errc::name_not_found));
}

bool of_protocol(const net::ip_addr& addr, net::protocol proto) noexcept
{
    switch (proto)
    {
    case net::protocol::not_care: return true;
    case net::protocol::ipv4: return addr.is_ipv4();
    case net::protocol::ipv6: return addr.is_ipv6();
    }
    return false;
}

// from_hosts is host's addresses in the hosts table (of proto's families: maybe none), if it's there.
std::optional<std::vector<net::ip_addr>> from_hosts(const dns::config& conf, std::string_view host, net::protocol proto)
{
    if (host.ends_with('.')) host.remove_suffix(1);

    const auto it = conf.hosts.find(host);
    if (it == conf.hosts.end()) return std::nullopt;

    std::vector<net::ip_addr> addrs;
    for (const auto& addr : it->second)
    {
        if (of_protocol(addr, proto)) addrs.push_back(addr);
    }
    return addrs;
}

}

namespace net::dns::detail
{

std::vector<std::string> name_list(const config& conf, std::string_view name)
{
    std::vector<std::string> names;

    if (name.ends_with('.'))
    {
        name.remove_suffix(1);
        if (sendable(name) && !avoid_dns(name)) names.emplace_back(name);
        return names;
    }

    const bool has_ndots = std::cmp_greater_equal(std::ranges::count(name, '.'), conf.ndots);

    if (has_ndots && sendable(name) && !avoid_dns(name)) names.emplace_back(name);

    for (const auto& suffix : conf.search)
    {
        auto fqdn = std::string{name};
        fqdn += '.';
        fqdn += suffix;
        if (sendable(fqdn) && !avoid_dns(fqdn)) names.push_back(std::move(fqdn));
    }

    if (!has_ndots && sendable(name) && !avoid_dns(name)) names.emplace_back(name);

    return names;
}

std::uint16_t random_id() noexcept
{
    constexpr std::size_t batch = 64;

    thread_local std::array<std::uint16_t, batch> ids{};
    thread_local std::size_t                      left = 0;

    if (left == 0)
    {
        fill_random(std::as_writable_bytes(std::span{ids}));
        left = ids.size();
    }

    return ids[--left];
}

coro::task<std::expected<std::vector<ip_addr>, std::error_code>> native_lookup(io::scheduler*                scheduler,
                                                                               std::shared_ptr<const config> conf,
                                                                               std::string                   host,
                                                                               protocol                      proto,
                                                                               std::size_t     first_server,
                                                                               std::stop_token stop)
{
    // the hosts table first, unless nsswitch.conf puts DNS first.
    if (conf->order == lookup_order::files_dns || conf->order == lookup_order::files)
    {
        auto found = from_hosts(*conf, host, proto);
        if (found.has_value() && !found->empty()) co_return std::move(*found);

        if (conf->order == lookup_order::files)
        {
            co_return std::unexpected(make_error_code(found.has_value() ? errc::no_address : errc::name_not_found));
        }
    }

    auto result = co_await query_dns(scheduler, *conf, host, proto, first_server, stop);

    if (!result.has_value() && !canceled(result.error()) && conf->order == lookup_order::dns_files)
    {
        auto found = from_hosts(*conf, host, proto);
        if (found.has_value() && !found->empty()) co_return std::move(*found);
    }

    co_return result;
}

}
