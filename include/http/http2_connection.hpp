#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread> // IWYU pragma: keep (std::this_thread::get_id, in debug builds)
#include <unordered_map>
#include <vector>

#include <spdlog/logger.h>

#include "coro/async_scope.hpp"
#include "coro/notifier.hpp"
#include "coro/task.hpp"
#include "coro/thread_pool.hpp"
#include "http/connection.hpp"
#include "http/headers.hpp"
#include "http/hpack.hpp"
#include "http/http2.hpp"
#include "http/http2_config.hpp"
#include "http/http2_session.hpp"
#include "io/io.hpp"
#include "io/reader.hpp"
#include "io/scheduler.hpp"
#include "io/writer.hpp"

// What HTTP/2's server and client connections share (see docs/plans/http2.md): not for applications.
namespace net::http::detail
{

class http2_connection_base;
class http2_stream_base;

// connection_mutex is a connection's lock: a std::mutex that, in debug builds, knows whether the calling thread holds
// it, so that stop sources are checked to be stopped without it (see http2_connection_base::stop_pending()).
class connection_mutex
{
public:
    void lock()
    {
        mu.lock();
#ifndef NDEBUG
        owner.store(std::this_thread::get_id(), std::memory_order::relaxed);
#endif
    }

    bool try_lock()
    {
        const bool locked = mu.try_lock();
#ifndef NDEBUG
        if (locked) owner.store(std::this_thread::get_id(), std::memory_order::relaxed);
#endif
        return locked;
    }

    void unlock()
    {
#ifndef NDEBUG
        owner.store(std::thread::id{}, std::memory_order::relaxed);
#endif
        mu.unlock();
    }

    // held is whether the calling thread holds the lock (in debug builds; always false otherwise).
    // NOTE: relaxed is enough: only this thread stores its own id, and it clears it before unlocking.
    [[nodiscard]] bool held() const noexcept
    {
#ifndef NDEBUG
        return owner.load(std::memory_order::relaxed) == std::this_thread::get_id();
#else
        return false;
#endif
    }

private:
    std::mutex mu;
#ifndef NDEBUG
    std::atomic<std::thread::id> owner{};
#endif
};

// stream_reader reads what a stream receives (a request's body, or a response's), as its DATA arrives, giving the
// peer back its windows as it goes.
class stream_reader final : public io::reader
{
public:
    explicit stream_reader(http2_stream_base* stream) noexcept
        : stream{stream}
    {}

    coro::task<io::result> read(std::span<std::byte> data) override;
    using io::reader::read;

    [[nodiscard]] io::handle native_handle() const noexcept override;

    // call_at_start sets what's awaited before the first read into a non-empty span reads anything (see
    // io::start_callback): e.g. the server's 100 Continue. It's awaited without the connection's lock.
    void call_at_start(io::start_callback callback) noexcept { at_start = callback; }

private:
    http2_stream_base* stream;
    io::start_callback at_start;
};

// stream_writer writes what a stream sends (a response's body, or a request's) as DATA, waiting for the
// flow-control windows to have room.
class stream_writer final : public io::writer
{
public:
    explicit stream_writer(http2_stream_base* stream) noexcept
        : stream{stream}
    {}

    coro::task<io::result> write(std::span<const std::byte> data) override;
    using io::writer::write;

    [[nodiscard]] io::handle native_handle() const noexcept override;

private:
    http2_stream_base* stream;
};

// http2_stream_base is what a stream's reader and writer use: what it's received, and how it's ended. Its fields are
// guarded by its connection's lock.
class http2_stream_base
{
public:
    http2_stream_base(http2_connection_base* conn, http2::stream_id id, coro::thread_pool* pool)
        : conn{conn}
        , id{id}
        , changed{pool}
    {}

    http2_stream_base(const http2_stream_base&)            = delete;
    http2_stream_base& operator=(const http2_stream_base&) = delete;
    http2_stream_base(http2_stream_base&&)                 = delete;
    http2_stream_base& operator=(http2_stream_base&&)      = delete;
    ~http2_stream_base()                                   = default;

    http2_connection_base* conn;
    http2::stream_id       id;

    std::vector<std::byte> received; // DATA not read yet, from received_start (before it, what's read)
    std::size_t            received_start = 0;
    bool                   received_end   = false;

    // The trailers received (those that may be: see trailer_allowed()), held until the reader reaches the end of what
    // was received, which moves them to trailers_to, if it's set (e.g. a request's): so whoever looks at them does on
    // the reader's task, once it's read to the end, never as the connection's reader adds them. Made only for a
    // message that has them, so a stream without any isn't larger for them.
    std::unique_ptr<headers> received_trailers;
    headers*                 trailers_to = nullptr;

    // hand_over_trailers moves the trailers received to trailers_to, if both are there (holding the lock).
    void hand_over_trailers() noexcept;

    bool            ended = false; // this side has sent END_STREAM
    std::error_code failure;       // reset, timed out, or the connection ended (see fail_stream())

    // The stop source of the stream's handler's token (server_request::stop): the server's streams have one, the
    // client's none. It's stopped when the stream fails, once the lock's released (see fail_stream()).
    std::stop_source stop{std::nostopstate};

    // When the stream times out, if it makes no progress: receiving (a client's response's head, then the body), and
    // sending. A wait sets its deadline if there's none, and only progress clears it (what's awaited arriving, or the
    // window growing), so waking for anything else doesn't put the timeout off.
    std::optional<io::clock::time_point> read_deadline;
    std::optional<io::clock::time_point> write_deadline;

    // How long the reader's and writer's waits may go without progress (0: no limit).
    std::chrono::milliseconds read_timeout{};
    std::chrono::milliseconds write_timeout{};

    // notified whenever any of the above changes, or the stream's (or connection's) window grows.
    coro::notifier changed;

    stream_writer writer{this};
};

// trim_value is value without the spaces and tabs around it, which HTTP/2 doesn't allow (RFC 9113, section 8.2.1),
// where HTTP/1.1 has them as optional whitespace (which its parsers strip, so they mean nothing).
[[nodiscard]] std::string_view trim_value(std::string_view value) noexcept;

// field_section is which section of a message append_fields() appends.
enum class field_section : std::uint8_t
{
    response, // a response's head
    request,  // a request's head: without Host, which :authority replaces, and TE unless it's "trailers"
    trailers, // either's trailers: without the fields that can't be trailers (see trailer_allowed())
};

// append_fields appends from's fields to fields as HTTP/2 sends them: names lowercased (into names, which fields views,
// so names mustn't be touched until fields are sent), values without the whitespace around them (see trim_value),
// without connection-specific fields (nor those Connection names; nor what section leaves out), and with credentials
// never indexed. It fails with std::errc::invalid_argument, appending nothing, if a field's name or value isn't valid
// (see valid_fields), as HTTP/1.1's encoder does: any field's, even one it would leave out.
[[nodiscard]] std::error_code append_fields(const headers&             from,
                                            std::vector<std::string>&  names,
                                            std::vector<hpack::field>& fields,
                                            field_section              section = field_section::response);

// http2_connection_base runs an HTTP/2 connection's session: a reader feeding it what's read, a writer writing its
// output, and a timer for its streams' timeouts (and the connection's, which a derived class says).
//
// The session's events run under the lock, as do the streams' readers and writers, briefly; so an event mustn't take
// the lock, nor resume anything that may (notifiers resume their waiters on the pool).
class http2_connection_base : protected http2::session_events
{
public:
    http2_connection_base(io::scheduler*               scheduler,
                          connection&                  conn,
                          http2::role                  side,
                          const http2::session_config& config,
                          spdlog::logger&              logger);

    http2_connection_base(const http2_connection_base&)            = delete;
    http2_connection_base& operator=(const http2_connection_base&) = delete;
    http2_connection_base(http2_connection_base&&)                 = delete;
    http2_connection_base& operator=(http2_connection_base&&)      = delete;
    ~http2_connection_base() override                              = default;

    // Used by the streams, holding the lock.
    connection_mutex mu;
    http2::session   session;

    // wake_writer tells the writer there's output, if there is: called without the lock.
    void wake_writer();

    // arm_deadline sets one of a stream's deadlines for a wait, timeout from now, unless it's set already (holding the
    // lock): telling the timer, if it's sooner than it knew.
    void arm_deadline(std::optional<io::clock::time_point>& deadline, std::chrono::milliseconds timeout);

    // output_drained is notified as the writer writes.
    coro::notifier& output_drained() noexcept { return drained; }

    // fail_stream fails s with err, unless it has failed already (holding the lock), queuing its stop source (if it
    // has one) for stop_pending() to stop. Waking its waits is the caller's.
    //
    // Stop sources are never stopped holding the lock: std::stop_source::request_stop() runs the callbacks registered
    // on it inline, and they're anyone's (the handler's, a scheduler wait's, ...), taking locks of their own. So each
    // place that may fail a stream calls stop_pending() once it's released the lock: the reader after receiving, the
    // timer after its checks, the writer and run() after end(), and a stream's own failed write. A copy of the source
    // is queued, rather than the stream, as the stream may be gone by then.
    void fail_stream(http2_stream_base& s, std::error_code err);

    // stop_pending stops the sources fail_stream() queued (without the lock).
    void stop_pending();

    [[nodiscard]] io::handle native_handle() const noexcept { return detail::native_handle(conn); }

protected:
    // run runs the connection until it ends (when the peer closes it, or it fails, or end() is called): already_read
    // is what was read of it before. Once it's ending, the writer writes what's left and shuts the connection down
    // (shutdown(): close_notify over TLS, then FIN), so the peer sees it end at once, while the reader goes on reading
    // (and dropping) what the peer still sends, until it closes its side too, or for linger_time; then it waits for the
    // writer and the timer, and only then awaits finish_streams() (e.g. the server's handlers, which may take a while).
    //
    // NOTE: the reader lingers as closing a socket with input unread (or, on macOS, input arriving after shutting down
    //       its reading side) resets the connection, and a reset discards what's still on its way to the peer: e.g.
    //       the end of a response, while the client sends WINDOW_UPDATEs for what it's read of it.
    coro::task<> run(std::span<const std::byte> already_read);

    // add_stream and remove_stream register a stream for the events and timeouts (holding the lock). remove_stream
    // gives the peer back what the stream received but wasn't read.
    void add_stream(http2_stream_base& s);
    void remove_stream(http2::stream_id id);

    // after_receive is called (without the lock) after each time the session's received something.
    virtual void after_receive() {}

    // finish_streams waits for the streams to be done, once the connection's ending.
    virtual coro::task<> finish_streams() { co_return; }

    // check_connection acts on the connection's own timeouts (holding the lock), returning when it next has to.
    virtual std::optional<io::clock::time_point> check_connection(io::clock::time_point now) = 0;

    // maybe_end ends the connection if it's done (holding the lock): called after receiving.
    virtual void maybe_end() {}

    // end ends the connection (holding the lock): its streams fail (with why: std::errc::connection_aborted, unless
    // something says more, e.g. a TLS alert the peer sent), the writer finishes (shutting the connection down), and
    // the reader stops receiving (lingering: see run()).
    void               end(std::error_code why = std::make_error_code(std::errc::connection_aborted));
    [[nodiscard]] bool ending() const noexcept { return is_ending; }

    void wake_timer();

    // The events every connection handles alike (with the lock held).
    void on_data(http2::stream_id id, std::span<const std::byte> data, bool end_stream) override;
    void on_stream_reset(http2::stream_id id, std::error_code err) override;
    void on_window_update(http2::stream_id id) override;
    void on_settings(const http2::settings& peer) override;

    // stop_all stops sources (without the lock: it checks, in debug builds).
    void stop_all(std::vector<std::stop_source>& sources) const;

    io::scheduler*     scheduler;
    coro::thread_pool* pool;
    connection&        conn;
    spdlog::logger&    logger;

    // Guarded by mu.
    std::unordered_map<http2::stream_id, http2_stream_base*> active;
    std::vector<std::stop_source>                            to_stop; // for stop_pending()

private:
    bool                                 receive(std::vector<std::byte>& pending);
    coro::task<>                         write_loop();
    coro::task<>                         timer_loop();
    std::optional<io::clock::time_point> check_streams(io::clock::time_point now);

    // Guarded by mu.
    bool                                 is_ending   = false;
    bool                                 reader_done = false; // it's stopped lingering
    io::clock::time_point                linger_until;        // when the timer stops the reader, once ending
    std::optional<io::clock::time_point> timer_target;

    coro::notifier             writer_wakeup; // output to write, or the connection's ending
    coro::notifier             drained;       // the writer has written some
    std::atomic<std::uint64_t> timer_word{0}; // the timer's, for scheduler::wait_on()

    coro::async_scope workers; // the writer and the timer
};

}
