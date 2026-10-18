#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <system_error>
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

private:
    http2_stream_base* stream;
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
    bool                   ended          = false; // this side has sent END_STREAM
    std::error_code        failure;                // reset, timed out, or the connection ended

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

// append_fields appends from's fields to fields as HTTP/2 sends them: names lowercased (into names, which fields views,
// so names mustn't be touched until fields are sent), without connection-specific fields (nor those Connection names;
// and, for a request, Host, which :authority replaces, and TE unless it's "trailers"), and with credentials never
// indexed. It fails with std::errc::invalid_argument, appending nothing, if a field's name or value isn't valid (see
// valid_fields), as HTTP/1.1's encoder does.
[[nodiscard]] std::error_code append_fields(const headers&             from,
                                            std::vector<std::string>&  names,
                                            std::vector<hpack::field>& fields,
                                            bool                       request = false);

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
    std::mutex     mu;
    http2::session session;

    // wake_writer tells the writer there's output, if there is: called without the lock.
    void wake_writer();

    // arm_deadline sets one of a stream's deadlines for a wait, timeout from now, unless it's set already (holding the
    // lock): telling the timer, if it's sooner than it knew.
    void arm_deadline(std::optional<io::clock::time_point>& deadline, std::chrono::milliseconds timeout);

    // output_drained is notified as the writer writes.
    coro::notifier& output_drained() noexcept { return drained; }

    [[nodiscard]] io::handle native_handle() const noexcept { return detail::native_handle(conn); }

protected:
    // run runs the connection until it ends (when the peer closes it, or it fails, or end() is called): already_read
    // is what was read of it before. Once the reader's done, it awaits finish_streams() (e.g. the server's handlers),
    // then lets the writer write what's left, and waits for it and the timer.
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

    // end ends the connection (holding the lock): its streams fail, the writer finishes, and the reader stops.
    void               end();
    [[nodiscard]] bool ending() const noexcept { return is_ending; }

    void wake_timer();

    // The events every connection handles alike (with the lock held).
    void on_data(http2::stream_id id, std::span<const std::byte> data, bool end_stream) override;
    void on_stream_reset(http2::stream_id id, std::error_code err) override;
    void on_window_update(http2::stream_id id) override;
    void on_settings(const http2::settings& peer) override;

    io::scheduler*     scheduler;
    coro::thread_pool* pool;
    connection&        conn;
    spdlog::logger&    logger;

    // Guarded by mu.
    std::unordered_map<http2::stream_id, http2_stream_base*> active;

private:
    bool                                 receive(std::vector<std::byte>& pending);
    coro::task<>                         write_loop();
    coro::task<>                         timer_loop();
    std::optional<io::clock::time_point> check_streams(io::clock::time_point now);

    // Guarded by mu.
    bool                                 is_ending = false;
    std::optional<io::clock::time_point> timer_target;

    coro::notifier             writer_wakeup; // output to write, or the connection's ending
    coro::notifier             drained;       // the writer has written some
    std::atomic<std::uint64_t> timer_word{0}; // the timer's, for scheduler::wait_on()

    coro::async_scope workers; // the writer and the timer
};

}
