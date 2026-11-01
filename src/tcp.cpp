#include "tcp.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <netdb.h>
#include <sys/socket.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include "coro/task.hpp"
#include "dns/resolver.hpp"
#include "exception.hpp"
#include "io/io.hpp"
#include "io/poll.hpp"
#include "io/scheduler.hpp"
#include "ip_addr.hpp"
#include "sockaddr.hpp"
#include "socket.hpp"
#include "util/defer.hpp"
#include "util/linked_stop_source.hpp"

namespace
{

// port_number is port as a number, if it is one (from 0 to 65535): service names (e.g. "http") aren't looked up.
std::optional<std::uint16_t> port_number(const std::string& port) noexcept
{
    std::uint16_t number = 0;

    const auto* end    = port.data() + port.size();
    auto [ptr, failed] = std::from_chars(port.data(), end, number);
    if (port.empty() || failed != std::errc{} || ptr != end) return std::nullopt;

    return number;
}

// zoned is the address of host, an IPv6 address with a zone (e.g. "fe80::1%en0"), which ip_addr (and so the resolver)
// doesn't have, so it's read by getaddrinfo(), which takes the zone's name or number. As it's numeric, nothing is
// looked up, so it doesn't block.
std::expected<net::detail::socket_address, std::error_code> zoned(const std::string& host, std::uint16_t port) noexcept
{
    addrinfo hints{};
    hints.ai_family   = AF_INET6;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags    = AI_NUMERICHOST;

    addrinfo* found = nullptr;
    if (auto err = ::getaddrinfo(host.c_str(), nullptr, &hints, &found); err != 0)
        return std::unexpected(net::make_gai_error_code(err));

    const std::unique_ptr<addrinfo, decltype(&::freeaddrinfo)> owned{found, &::freeaddrinfo};
    if (found == nullptr || found->ai_addrlen > sizeof(sockaddr_storage) || found->ai_family != AF_INET6)
        return std::unexpected(std::make_error_code(std::errc::invalid_argument));

    net::detail::socket_address addr;
    std::memcpy(&addr.storage, found->ai_addr, found->ai_addrlen);
    addr.size = found->ai_addrlen;

    auto* sin6      = reinterpret_cast<sockaddr_in6*>(&addr.storage);
    sin6->sin6_port = htons(port);
    return addr;
}

bool of_protocol(const net::detail::socket_address& addr, net::protocol proto) noexcept
{
    switch (proto)
    {
    case net::protocol::not_care: return addr.family() == AF_INET || addr.family() == AF_INET6;
    case net::protocol::ipv4: return addr.family() == AF_INET;
    case net::protocol::ipv6: return addr.family() == AF_INET6;
    }

    return false;
}

std::error_code from_errno(int err) noexcept { return {err, std::system_category()}; }

// wait_connected waits for a connection that's in progress to be established, returning the error if it fails.
//
// The connection is established (or failed) once the socket is writable. The socket may be reported ready before then
// (e.g. as Linux reports a socket that isn't connected yet as hung up, when it's registered), so that's checked, and it
// waits again if it's still connecting.
net::coro::task<std::error_code> wait_connected(net::io::scheduler*        scheduler,
                                                net::io::handle            fd,
                                                net::io::clock::time_point deadline,
                                                std::stop_token            stop)
{
    while (true)
    {
        if (auto err = co_await scheduler->wait_until(fd, net::io::poll_op::write, deadline, stop); err) co_return err;

        int       sock_err = 0;
        socklen_t size     = sizeof(sock_err);
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &sock_err, &size) == -1) sock_err = errno;
        if (sock_err != 0) co_return from_errno(sock_err);

        sockaddr_storage peer{};
        socklen_t        peer_size = sizeof(peer);
        if (::getpeername(fd, reinterpret_cast<sockaddr*>(&peer), &peer_size) == 0) co_return std::error_code{};
        if (errno != ENOTCONN) co_return from_errno(errno);
    }
}

// started is an attempt to connect that has started: its socket, and whether it's connected already.
struct started
{
    net::tcp_socket conn;
    bool            connected = false;
};

// start starts connecting to addr: it fails if connecting failed at once.
std::expected<started, std::error_code> start(net::io::scheduler*                     scheduler,
                                              const net::detail::socket_address&      addr,
                                              const net::tcp_socket::connect_options& options)
{
    net::io::handle fd = ::socket(addr.family(), SOCK_STREAM, 0);
    if (fd == -1) return std::unexpected(from_errno(errno));

    // makes the socket non-blocking before connecting, and owns it from here on.
    net::tcp_socket conn{scheduler, fd};

    if (options.keepalive)
    {
        const int yes = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof(yes));
    }

    std::error_code err;
    if (options.system_connect) err = options.system_connect(fd, addr.get(), addr.size);
    else if (::connect(fd, addr.get(), addr.size) == -1) err = from_errno(errno);

    if (!err) return started{.conn = std::move(conn), .connected = true};
    if (err == std::errc::operation_in_progress) return started{.conn = std::move(conn), .connected = false};

    conn.close(false);
    return std::unexpected(err);
}

// connected is conn, which has connected (and won: it's what connect returns), with Nagle's algorithm off
// (TCP_NODELAY), as Go, curl and Chromium set it on their clients' connections. With it on, a write that doesn't fill a
// segment waits until what was sent before is acknowledged, and a peer that delays its acknowledgements (Linux's, by
// 40 ms or more) holds it up that long: e.g. an upload stalls at the end of each flow-control window, and a request's
// head written apart from its body waits. Set on the winner alone, not on each attempt. A failure to set it is
// ignored: the connection works either way, and a caller who wants Nagle's algorithm calls set_nodelay(false).
net::tcp_socket connected(net::tcp_socket conn) noexcept
{
    [[maybe_unused]] const auto set = conn.set_nodelay();
    return conn;
}

// one_at_a_time connects to the first of addresses that accepts, trying them in order, by deadline, each attempt
// getting its share of what's left (see net::detail::attempt_deadline()).
net::coro::task<std::expected<net::tcp_socket, std::error_code>>
one_at_a_time(net::io::scheduler*                      scheduler,
              std::vector<net::detail::socket_address> addresses,
              net::io::clock::time_point               deadline,
              net::tcp_socket::connect_options         options)
{
    std::error_code last_err;

    for (std::size_t i = 0; i < addresses.size(); ++i)
    {
        if (options.stop.stop_requested())
            co_return std::unexpected(std::make_error_code(std::errc::operation_canceled));

        // an attempt that timed out took its share, and the time may be up for the rest too.
        const auto now = net::io::clock::now();
        if (now >= deadline) co_return std::unexpected(std::make_error_code(std::errc::timed_out));

        auto attempt = start(scheduler, addresses[i], options);
        if (!attempt.has_value())
        {
            last_err = attempt.error();
            continue;
        }
        if (attempt->connected) co_return connected(std::move(attempt->conn));

        auto& conn   = attempt->conn;
        auto  result = co_await wait_connected(scheduler,
                                               conn.native_handle(),
                                               net::detail::attempt_deadline(now, deadline, addresses.size() - i),
                                               options.stop);
        if (!result) co_return connected(std::move(conn));

        conn.close(false);

        // a timeout only ends this attempt, its share of the time being up: the rest go on with what's left.
        if (result == std::errc::operation_canceled) co_return std::unexpected(result);

        last_err = result;
    }

    co_return std::unexpected(last_err);
}

// interleave orders addresses by family alternately, the first address's family first, keeping each family's in their
// order (RFC 8305 section 4, with a First Address Family Count of 1): e.g. 6a 6b 4a 4b 6c is 6a 4a 6b 4b 6c.
void interleave(std::vector<net::detail::socket_address>& addresses) noexcept
{
    for (auto it = addresses.begin(); it != addresses.end() && std::next(it) != addresses.end(); ++it)
    {
        const auto other = it->family() == AF_INET ? AF_INET6 : AF_INET;
        const auto found =
            std::find_if(std::next(it), addresses.end(), [&](const auto& a) { return a.family() == other; });
        if (found == addresses.end()) return;

        // the other family's next address comes next, the rest moving up behind it.
        std::rotate(std::next(it), found, std::next(found));
    }
}

// same is whether a and b are the same address (and port).
bool same(const net::detail::socket_address& a, const net::detail::socket_address& b) noexcept
{
    return a.size == b.size && std::memcmp(a.get(), b.get(), a.size) == 0;
}

// sort_in puts found's addresses (in the resolver's order) that the attempts haven't tried yet (those before next)
// after them, interleaved by family as interleave() does, but going on from the last tried (RFC 8305 section 4): the
// other family's first.
void sort_in(std::vector<net::detail::socket_address>& addresses,
             std::size_t                               next,
             const std::vector<net::ip_addr>&          found,
             std::uint16_t                             port)
{
    std::vector<net::detail::socket_address> rest;
    rest.reserve(found.size());
    for (const auto& addr : found)
    {
        auto       converted = net::detail::to_sockaddr(addr, port);
        const auto tried     = std::any_of(addresses.begin(),
                                           addresses.begin() + static_cast<std::ptrdiff_t>(next),
                                           [&](const auto& a) { return same(a, converted); });
        if (!tried) rest.push_back(converted);
    }

    if (next > 0)
    {
        const auto other = addresses[next - 1].family() == AF_INET ? AF_INET6 : AF_INET;
        const auto it    = std::ranges::find_if(rest, [&](const auto& a) { return a.family() == other; });
        if (it != rest.end()) std::rotate(rest.begin(), it, std::next(it));
    }
    interleave(rest);

    addresses.resize(next);
    addresses.insert(addresses.end(), rest.begin(), rest.end());
}

// pending is the host's lookup while the attempts have started on some of its addresses but its other family's are
// still to come (RFC 8305 section 3): what's found later is sorted into the attempts not yet made.
struct pending
{
    net::dns::detail::resolution* resolution = nullptr; // null once the lookup is done
    std::uint64_t                 merged     = 0;       // the version of what it had found that's been sorted in
    std::uint64_t                 watched    = 0;       // the version its race's word last watched, if it does
    std::uint16_t                 port       = 0;

    // merge sorts in what the lookup has found since it last did (if anything), returning whether more may come.
    bool merge(std::vector<net::detail::socket_address>& addresses, std::size_t next)
    {
        if (resolution == nullptr || resolution->news().load(std::memory_order::acquire) == merged)
            return resolution != nullptr;

        auto found = resolution->look();
        merged     = found.version;
        if (found.done) resolution = nullptr;
        if (!found.addrs.empty()) sort_in(addresses, next, found.addrs, port);
        return resolution != nullptr;
    }

    // waiting is whether the lookup may still find more.
    [[nodiscard]] bool waiting() const noexcept { return resolution != nullptr; }

    // wait waits until the lookup finds something new, by deadline, or stop.
    [[nodiscard]] net::io::scheduler::stoppable_wait_operation
    wait(net::io::scheduler* scheduler, net::io::clock::time_point deadline, std::stop_token stop) const noexcept
    {
        return scheduler->wait_on(resolution->news(), merged, deadline, std::move(stop));
    }

    // watch has the lookup bump word (and notify it) too when it next finds something, returning false if it has
    // since it was last merged (then merge it first).
    bool watch(std::shared_ptr<std::atomic<std::uint64_t>> word)
    {
        if (watched == merged) return true;
        if (!resolution->watch(std::move(word), merged)) return false;
        watched = merged;
        return true;
    }
};

// more_coming sorts in what more (if any) has found (see pending::merge()), returning whether more may come.
bool more_coming(pending* more, std::vector<net::detail::socket_address>& addresses, std::size_t next)
{
    return more != nullptr && more->merge(addresses, next);
}

// none_left is whether there are no addresses left to try after next (of count), nor any to come (waiting).
bool none_left(std::size_t next, std::size_t count, bool waiting) noexcept { return next == count && !waiting; }

// after is delay after now, but no later than deadline (after now).
net::io::clock::time_point
after(net::io::clock::time_point now, std::chrono::milliseconds delay, net::io::clock::time_point deadline) noexcept
{
    // NOTE: compared in milliseconds, so a long delay (e.g. milliseconds::max()) doesn't overflow.
    if (delay >= std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)) return deadline;
    return now + delay;
}

// inline_until is when an attempt waited for inline (started at now) gives way to the next: after the attempt delay,
// unless it's the last (then the deadline).
net::io::clock::time_point inline_until(net::io::clock::time_point now,
                                        bool                       last,
                                        std::chrono::milliseconds  delay,
                                        net::io::clock::time_point deadline) noexcept
{
    return last ? deadline : after(now, delay, deadline);
}

// race is what a race of attempts to connect shares: racing() (and keep_racing(), which it hands the race to) and the
// attempts spawned (see racing_attempt()), each holding it, so an attempt may outlive them (e.g. a loser that's still
// being stopped). The attempts tell keep_racing() what happened (under mu), then bump changes, which it waits on.
struct race
{
    race(net::io::scheduler* scheduler, const std::stop_token& stop)
        : scheduler{scheduler}
        , stop{stop}
    {}

    // claim makes conn the race's winner, if there's none yet and the race isn't over, returning whether it did (and
    // then notifies keep_racing()).
    bool claim(net::tcp_socket& conn) noexcept
    {
        {
            const std::scoped_lock lock{mu};
            if (over || winner.has_value()) return false;
            winner.emplace(std::move(conn));
        }

        changed();
        return true;
    }

    // fail records that the index'th attempt (spawned) failed with err, and notifies keep_racing().
    void fail(std::size_t index, std::error_code err) noexcept
    {
        {
            const std::scoped_lock lock{mu};
            ++failed;
            latest_failed = std::max(latest_failed, index + 1);
            last_err      = err;
        }

        changed();
    }

    // failed_at_once records that an attempt keep_racing() started failed at once, with err.
    void failed_at_once(std::error_code err) noexcept
    {
        const std::scoped_lock lock{mu};
        last_err = err;
    }

    // view is what keep_racing() sees of the race.
    struct view
    {
        // the winner, which keep_racing() then has (and the race is over).
        std::optional<net::tcp_socket> winner;
        // how many of the attempts spawned are still going, and whether the latest has failed.
        std::size_t in_flight     = 0;
        bool        latest_failed = false;
        // the error of the last attempt to fail, if any has.
        std::error_code last_err;
    };

    // look is the race as it is, of spawned attempts, next being the index of the next to start: so
    // latest_failed is about the attempt before.
    view look(std::size_t spawned, std::size_t next) noexcept
    {
        const std::scoped_lock lock{mu};

        view now;
        if (winner.has_value())
        {
            now.winner = std::exchange(winner, std::nullopt);
            over       = true;
        }
        now.in_flight     = spawned - failed;
        now.latest_failed = latest_failed == next;
        now.last_err      = last_err;
        return now;
    }

    // end ends the race (racing() returning, or its frame destroyed): a winner not taken is closed, as are any later
    // ones (by their attempts), and the attempts still going are stopped. It holds none of the scheduler's locks (as
    // stopping must not).
    void end() noexcept
    {
        std::optional<net::tcp_socket> unclaimed;
        {
            const std::scoped_lock lock{mu};
            over      = true;
            unclaimed = std::exchange(winner, std::nullopt);
        }

        stop.request_stop();
    }

    net::io::scheduler* const scheduler;

    // stop stops the attempts: requested by end(), and when connect's stop is.
    net::util::linked_stop_source stop;

    // changes is bumped (and notified) when an attempt ends: what keep_racing() waits on.
    std::atomic<std::uint64_t> changes{0};

    std::mutex mu;

    // guarded by mu:
    // the first attempt to connect, until keep_racing() takes it.
    std::optional<net::tcp_socket> winner;
    // whether keep_racing() has taken the winner, or racing() has returned, so a later connection is closed.
    bool over = false;
    // how many of the spawned attempts failed; one more than the index of the latest of them to (0: none has).
    std::size_t failed        = 0;
    std::size_t latest_failed = 0;
    // the error of the last attempt to fail (spawned, or not: racing()'s before the race, keep_racing()'s at once).
    std::error_code last_err;

    // changed tells keep_racing() that something changed.
    void changed() noexcept
    {
        changes.fetch_add(1, std::memory_order::release);
        scheduler->notify_all(changes);
    }
};

// watch_more has more's lookup (if any, and if it may find more) bump the race's word too (see pending::watch()),
// returning false if it has found something since it was last merged.
bool watch_more(pending* more, const std::shared_ptr<race>& state)
{
    return more == nullptr || !more->waiting() || more->watch({state, &state->changes});
}

// racing_attempt waits for the index'th attempt, conn, to connect, by deadline, and tells the race.
net::coro::task<> racing_attempt(std::shared_ptr<race>      state,
                                 std::size_t                index,
                                 net::tcp_socket            conn,
                                 net::io::clock::time_point deadline)
{
    auto err = co_await wait_connected(state->scheduler, conn.native_handle(), deadline, state->stop.get_token());
    if (!err && state->claim(conn)) co_return;

    // it lost (another attempt connected first, or the race is over), or failed.
    conn.close(false);
    if (err) state->fail(index, err);
}

// ended is why connecting has to end now, if it does: stop was requested (or the scheduler is stopping), or the
// deadline has passed.
std::error_code
ended(const net::io::scheduler* scheduler, const std::stop_token& stop, net::io::clock::time_point deadline) noexcept
{
    if (stop.stop_requested() || scheduler->stop_requested())
        return std::make_error_code(std::errc::operation_canceled);
    if (net::io::clock::now() >= deadline) return std::make_error_code(std::errc::timed_out);
    return {};
}

// keep_racing goes on with racing()'s race, once its first attempt has outlasted the delay (and is the race's): it
// starts the rest of addresses, from next (and those more sorts in, as the host's lookup finds them), and returns the
// first to connect.
net::coro::task<std::expected<net::tcp_socket, std::error_code>>
keep_racing(net::io::scheduler*                       scheduler,
            std::vector<net::detail::socket_address>& addresses,
            std::size_t                               next,
            net::io::clock::time_point                deadline,
            const net::tcp_socket::connect_options&   options,
            const std::shared_ptr<race>&              state,
            pending*                                  more)
{
    std::size_t spawned    = 1;                     // attempts handed to tasks
    auto        next_start = net::io::clock::now(); // the next is due at once

    while (true)
    {
        // read before looking at the race (and the lookup), so a change after is noticed by the wait.
        const auto seen    = state->changes.load(std::memory_order::acquire);
        const bool waiting = more_coming(more, addresses, next);

        auto now = state->look(spawned, next);
        if (now.winner.has_value()) co_return connected(std::move(*now.winner));

        if (auto err = ended(scheduler, options.stop, deadline); err) co_return std::unexpected(err);
        if (next == addresses.size() && now.in_flight == 0 && !waiting) co_return std::unexpected(now.last_err);

        const auto time = net::io::clock::now();
        const bool due  = now.in_flight == 0 || now.latest_failed || time >= next_start;
        if (next < addresses.size() && due)
        {
            const auto index   = next++;
            auto       attempt = start(scheduler, addresses[index], options);
            if (!attempt.has_value())
            {
                // the next starts at once.
                state->failed_at_once(attempt.error());
                next_start = time;
                continue;
            }
            if (attempt->connected) co_return connected(std::move(attempt->conn));

            // NOTE: refused (and so destroyed, closing the socket) only if the scheduler is stopping.
            if (!scheduler->spawn(racing_attempt(state, index, std::move(attempt->conn), deadline)))
                co_return std::unexpected(std::make_error_code(std::errc::operation_canceled));
            ++spawned;
            next_start = after(time, options.attempt_delay, deadline);
            continue;
        }

        // wait for an attempt to end (or the lookup to find more, which bumps the race's word too), until the next is
        // due (or the end).
        if (!watch_more(more, state)) continue; // it has: sort it in

        const auto until = next < addresses.size() ? std::min(next_start, deadline) : deadline;

        // an attempt ended, the time came, or stop: the loop's start tells which.
        (void)co_await scheduler->wait_on(state->changes, seen, until, options.stop);
    }
}

// racing connects to the first of addresses (interleaved by family) to accept, by deadline, starting an attempt every
// attempt_delay, or at once when the latest fails (RFC 8305 section 5): see connect_options::attempt_delay. If more
// (not null) is waiting for the host's other family, its addresses are sorted into those not tried yet once they come
// in, and the attempts wait for them (until the deadline) if all those so far have failed.
//
// An attempt waits here (inline) until the next is due: the first to connect often does, and then nothing is racing.
// One that outlasts it is handed to a task of its own (racing_attempt()), with the race's shared state (race), and from
// then on every attempt that doesn't finish at once is; this waits on the race's word, until the next attempt is due.
net::coro::task<std::expected<net::tcp_socket, std::error_code>>
racing(net::io::scheduler*                      scheduler,
       std::vector<net::detail::socket_address> addresses,
       net::io::clock::time_point               deadline,
       net::tcp_socket::connect_options         options,
       pending*                                 more = nullptr)
{
    interleave(addresses);

    // made once an attempt outlasts the delay, and ended (its attempts stopped) however this ends.
    std::shared_ptr<race> state;

    const net::util::defer end_race{[&state] noexcept
                                    {
                                        if (state) state->end();
                                    }};

    std::error_code last_err;
    std::size_t     next = 0; // the next address to try

    // one at a time until an attempt outlasts the delay: then it goes on, in a task, alongside the next.
    while (!state)
    {
        const bool waiting = more_coming(more, addresses, next);

        if (auto err = ended(scheduler, options.stop, deadline); err) co_return std::unexpected(err);
        if (none_left(next, addresses.size(), waiting)) co_return std::unexpected(last_err);
        if (next == addresses.size())
        {
            // every attempt so far has failed: wait for the other family's addresses (until the deadline, or stop: the
            // loop's start tells which).
            (void)co_await more->wait(scheduler, deadline, options.stop);
            continue;
        }

        const auto now     = net::io::clock::now();
        const auto index   = next++;
        auto       attempt = start(scheduler, addresses[index], options);
        if (!attempt.has_value())
        {
            last_err = attempt.error();
            continue;
        }
        if (attempt->connected) co_return connected(std::move(attempt->conn));

        // the last waits until the end (unless more may come).
        auto&      conn  = attempt->conn;
        const bool last  = none_left(next, addresses.size(), waiting);
        const auto until = inline_until(now, last, options.attempt_delay, deadline);

        auto err = co_await wait_connected(scheduler, conn.native_handle(), until, options.stop);
        if (!err) co_return connected(std::move(conn));

        if (err != std::errc::timed_out || last || net::io::clock::now() >= deadline)
        {
            // it failed (or the time's up), and the next starts at once.
            conn.close(false);
            if (err == std::errc::operation_canceled) co_return std::unexpected(err);
            last_err = err;
            continue;
        }

        state           = std::make_shared<race>(scheduler, options.stop);
        state->last_err = last_err;

        // NOTE: refused (and so destroyed, closing the socket) only if the scheduler is stopping.
        if (!scheduler->spawn(racing_attempt(state, index, std::move(conn), deadline)))
            co_return std::unexpected(std::make_error_code(std::errc::operation_canceled));
    }

    co_return co_await keep_racing(scheduler, addresses, next, deadline, options, state, more);
}

// connect_to connects to the first of addresses (those of options.proto's family) that accepts, by deadline: racing
// them (see racing()), unless options.attempt_delay is 0 or there's only one, then one at a time.
net::coro::task<std::expected<net::tcp_socket, std::error_code>>
connect_to(net::io::scheduler*                      scheduler,
           std::vector<net::detail::socket_address> addresses,
           net::io::clock::time_point               deadline,
           net::tcp_socket::connect_options         options)
{
    const auto given = addresses.size();
    std::erase_if(addresses, [&](const auto& addr) { return !of_protocol(addr, options.proto); });
    if (addresses.empty())
    {
        return net::coro::ready_task<std::expected<net::tcp_socket, std::error_code>>(std::unexpected(
            std::make_error_code(given == 0 ? std::errc::invalid_argument : std::errc::address_family_not_supported)));
    }

    if (options.attempt_delay <= std::chrono::milliseconds::zero() || addresses.size() == 1)
        return one_at_a_time(scheduler, std::move(addresses), deadline, std::move(options));

    return racing(scheduler, std::move(addresses), deadline, std::move(options));
}

// connect_resolving connects to a host's addresses as its lookup (res, of both families) finds them, by deadline
// (RFC 8305 section 3): starting on IPv6's at once, or on IPv4's once they've had the resolution delay (unless IPv6's
// come in by then), with what's found later sorted into the attempts not yet made (see racing()).
net::coro::task<std::expected<net::tcp_socket, std::error_code>>
connect_resolving(net::io::scheduler*              scheduler,
                  net::dns::detail::resolution     res,
                  std::uint16_t                    port,
                  net::io::clock::time_point       deadline,
                  net::tcp_socket::connect_options options)
{
    auto found = res.look();
    while (!found.done)
    {
        if (auto err = ended(scheduler, options.stop, deadline); err) co_return std::unexpected(err);

        auto until = deadline;
        if (!found.addrs.empty())
        {
            if (found.family == net::protocol::ipv6) break;

            // NOTE: no later than the deadline, which ends it first (at the loop's start).
            until = after(found.at, options.resolution_delay, deadline);
            if (net::io::clock::now() >= until) break;
        }

        // something new, or the delay's end (or the deadline's: the loop's start tells which).
        auto err = co_await scheduler->wait_on(res.news(), found.version, until, options.stop);
        if (err == std::errc::operation_canceled) co_return std::unexpected(err);

        found = res.look();
    }

    if (found.addrs.empty()) co_return std::unexpected(found.err);

    std::vector<net::detail::socket_address> addresses;
    addresses.reserve(found.addrs.size());
    for (const auto& addr : found.addrs) addresses.push_back(net::detail::to_sockaddr(addr, port));

    if (found.done) co_return co_await connect_to(scheduler, std::move(addresses), deadline, std::move(options));

    pending more{.resolution = &res, .merged = found.version, .port = port};
    co_return co_await racing(scheduler, std::move(addresses), deadline, std::move(options), &more);
}

}

namespace net
{

tcp_socket::tcp_socket(io::scheduler* scheduler) noexcept
    : socket{scheduler, invalid_fd}
{}

tcp_socket::tcp_socket(io::scheduler* scheduler, io::handle fd)
    : socket{scheduler, fd}
{}

std::expected<void, std::error_code> tcp_socket::set_nodelay(bool nodelay) const noexcept
{
    const int value = nodelay ? 1 : 0;
    if (::setsockopt(native_handle(), IPPROTO_TCP, TCP_NODELAY, &value, sizeof(value)) == -1)
        return std::unexpected(std::error_code{errno, std::system_category()});
    return {};
}

std::expected<void, std::error_code> tcp_socket::set_notsent_lowat(std::size_t bytes) const noexcept
{
#ifdef TCP_NOTSENT_LOWAT
    const int value = static_cast<int>(std::min<std::size_t>(bytes, std::numeric_limits<int>::max()));
    if (::setsockopt(native_handle(), IPPROTO_TCP, TCP_NOTSENT_LOWAT, &value, sizeof(value)) == -1)
        return std::unexpected(std::error_code{errno, std::system_category()});
    return {};
#else
    (void)bytes;
    return std::unexpected(std::make_error_code(std::errc::not_supported));
#endif
}

io::clock::time_point
detail::attempt_deadline(io::clock::time_point now, io::clock::time_point deadline, std::size_t remaining) noexcept
{
    // an equal share for each address left, unless that's too short to connect: then this one takes more (up to all of
    // what's left), at the expense of the last ones.
    constexpr io::clock::duration sane_minimum = std::chrono::seconds{2};

    const auto left  = deadline - now;
    auto       share = left / static_cast<io::clock::rep>(std::max<std::size_t>(remaining, 1));
    if (share < sane_minimum) share = std::min(left, sane_minimum);

    return now + share;
}

coro::task<std::expected<tcp_socket, std::error_code>>
tcp_socket::connect(io::scheduler* scheduler, std::string host, std::string port, connect_options options)
{
    if (options.stop.stop_requested()) co_return std::unexpected(std::make_error_code(std::errc::operation_canceled));

    const auto number = port_number(port);
    if (!number.has_value()) co_return std::unexpected(std::make_error_code(std::errc::invalid_argument));

    // the timeout covers resolving the host, as well as connecting.
    const auto deadline = io::clock::now() + options.timeout;

    std::vector<detail::socket_address> addresses;

    if (host.contains('%'))
    {
        auto addr = zoned(host, *number);
        if (!addr.has_value()) co_return std::unexpected(addr.error());
        addresses.push_back(*addr);
    }
    else
    {
        // NOTE: an IP address is returned at once, without a thread.
        const auto& resolver = options.resolver != nullptr ? *options.resolver : dns::default_resolver();

        // racing both families: started on the first's addresses (see connect_resolving()).
        if (options.proto == protocol::not_care && options.attempt_delay > std::chrono::milliseconds::zero())
        {
            auto res = resolver.start_resolving(scheduler, std::move(host), options.proto);
            if (!res.has_value()) co_return std::unexpected(res.error());

            co_return co_await connect_resolving(scheduler, std::move(*res), *number, deadline, std::move(options));
        }

        auto found = co_await resolver.resolve(scheduler,
                                               std::move(host),
                                               {.proto = options.proto, .deadline = deadline, .stop = options.stop});
        if (!found.has_value()) co_return std::unexpected(found.error());

        addresses.reserve(found->size());
        for (const auto& addr : *found) addresses.push_back(detail::to_sockaddr(addr, *number));
    }

    co_return co_await connect_to(scheduler, std::move(addresses), deadline, std::move(options));
}

coro::task<std::expected<tcp_socket, std::error_code>> tcp_socket::connect(io::scheduler*           scheduler,
                                                                           std::span<const ip_addr> addresses,
                                                                           std::uint16_t            port,
                                                                           const connect_options&   options)
{
    // NOTE: not a coroutine, so the addresses are converted (and so copied) before it returns.
    std::vector<detail::socket_address> converted;
    converted.reserve(addresses.size());
    for (const auto& addr : addresses) converted.push_back(detail::to_sockaddr(addr, port));

    return connect_to(scheduler, std::move(converted), io::clock::now() + options.timeout, options);
}

}
