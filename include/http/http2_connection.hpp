#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <expected>
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
#include "http/http2_scheduler.hpp"
#include "http/http2_session.hpp"
#include "http/tunnel.hpp"
#include "io/buffered_reader.hpp"
#include "io/buffered_writer.hpp"
#include "io/io.hpp"
#include "io/reader.hpp"
#include "io/scheduler.hpp"
#include "io/writer.hpp"

// What HTTP/2's server and client connections share: not for applications.
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

// received_chunk is a piece of what a stream's received and not yet read (see received_data): a default-size DATA
// frame's worth of bytes, and the link to the next piece, in one allocation. Its bytes aren't initialized: they're
// written before they're read.
// NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init): bytes, deliberately.
struct received_chunk
{
    static constexpr std::size_t capacity = std::size_t{16} * 1'024;

    received_chunk*                 next = nullptr;
    std::array<std::byte, capacity> bytes;
};

// chunk_pool is a connection's free received_chunks, which its streams take as DATA arrives and give back as they're
// read, so steady traffic reuses them rather than allocating. It keeps at most its limit (freeing the rest), which the
// connection sets from how many streams it has (see limit_for()), and the connection's timer trims it (see trim()), so
// an idle connection soon holds none. Guarded by the connection's lock; it frees what it keeps when it's destroyed.
class chunk_pool
{
public:
    // How many free chunks a connection keeps: 16 (256 KiB, what a tunnel's stream may hold unread, see stream_tunnel),
    // and one more for each of its streams, so busy streams reuse what one another give back. (Measured with
    // bench_websocket and bench_request: 4, or 4 and one a stream, had the streams of a busy connection allocate
    // chunks again and again, more than the vectors these replaced; more per stream gained nothing.)
    static constexpr std::size_t base_free       = 16;
    static constexpr std::size_t free_per_stream = 1;

    // limit_for is the limit for a connection with streams streams.
    [[nodiscard]] static constexpr std::size_t limit_for(std::size_t streams) noexcept
    {
        return base_free + (free_per_stream * streams);
    }

    chunk_pool() = default;

    chunk_pool(const chunk_pool&)            = delete;
    chunk_pool& operator=(const chunk_pool&) = delete;
    chunk_pool(chunk_pool&&)                 = delete;
    chunk_pool& operator=(chunk_pool&&)      = delete;
    ~chunk_pool();

    // take is a free chunk, or a new one if none is (whose bytes aren't initialized).
    [[nodiscard]] received_chunk* take();

    // give takes a chunk back: kept if fewer than the limit are, freed otherwise.
    void give(received_chunk* chunk) noexcept;

    // set_limit sets how many free chunks it keeps, freeing those past it.
    void set_limit(std::size_t n) noexcept;

    // trim frees the chunks it's kept since the last trim that none took (as many as the fewest it kept meanwhile),
    // returning whether it keeps any: called periodically, it keeps what traffic takes, and frees the rest within two
    // periods.
    bool trim() noexcept;

    // size is how many free chunks it keeps.
    [[nodiscard]] std::size_t size() const noexcept { return count; }

private:
    received_chunk* first  = nullptr;
    std::size_t     count  = 0;
    std::size_t     fewest = 0; // since the last trim
    std::size_t     limit  = limit_for(0);
};

// received_data is what a stream's received and not yet read: a list of chunks from its connection's chunk_pool, each
// given back as it's read to its end, so it holds no more than what's unread needs (an idle stream, nothing). DATA may
// span chunks, as may a read. It frees the chunks it still holds when it's destroyed (a stream may outlive its
// connection), so a stream that's done gives them back first (clear()), holding the connection's lock.
class received_data
{
public:
    received_data() = default;

    received_data(const received_data&)            = delete;
    received_data& operator=(const received_data&) = delete;
    received_data(received_data&&)                 = delete;
    received_data& operator=(received_data&&)      = delete;
    ~received_data();

    [[nodiscard]] bool        empty() const noexcept { return count == 0; }
    [[nodiscard]] std::size_t size() const noexcept { return count; }

    // append adds data after what's there, taking chunks from pool as it needs them.
    void append(std::span<const std::byte> data, chunk_pool& pool);

    // take moves as much as fits into into, from the front, giving the chunks it's read to their ends back to pool,
    // and returns how much it took.
    std::size_t take(std::span<std::byte> into, chunk_pool& pool) noexcept;

    // clear drops what's there, giving its chunks back to pool.
    void clear(chunk_pool& pool) noexcept;

private:
    received_chunk* head  = nullptr; // read from start
    received_chunk* tail  = nullptr; // written up to end
    std::size_t     start = 0;
    std::size_t     end   = 0;
    std::size_t     count = 0; // what's unread, over all the chunks
};

// stream_read is a stream_reader's read that has to wait for what its stream's to receive: an operation (see
// coro::operation), so a read that waits needs no coroutine. Its stream knows it while it waits (reading), and the
// connection's events finish it, holding the lock: DATA that comes meanwhile goes straight into its span (as much as
// fits: the rest, to what the stream's received), and the stream's end, its failure, or (a tunnel's) stop or time
// finish it too. Its reader's resumed on the pool, never by whoever finished it, and takes its result holding the lock:
// until then, DATA goes on into what's left of the span. The peer's given its window back as the reader takes what's
// come.
//
// As a socket's read, it must outlive a read waiting on it: a coroutine destroyed while it waits cancels it (cancel()),
// which is only safe while it hasn't finished (its reader not yet resumed).
class stream_read final : public coro::operation<io::result>
{
public:
    explicit stream_read(http2_stream_base* stream) noexcept
        : stream{stream}
    {}

    // claim readies it to read into into, returning false if it's in use.
    [[nodiscard]] bool claim(std::span<std::byte> into) noexcept;

    bool       start(std::coroutine_handle<> caller) noexcept override;
    io::result result() noexcept override;
    void       cancel() noexcept override;

    // take copies what fits of incoming into the read's span, unless it's finished for something else (holding the
    // lock, with nothing else received before it), returning what's left of incoming.
    std::span<const std::byte> take(std::span<const std::byte> incoming) noexcept;

    // wake finishes the read if it has something to return (holding the lock), resuming its reader on the pool.
    void wake() noexcept;

private:
    // ready is whether the read has something to return (holding the lock): what's come into its span, or what its
    // stream received, or else why it's done (err: the stream's end, its failure, or a tunnel's stop or time).
    bool ready();

    http2_stream_base*                   stream;
    std::span<std::byte>                 data;
    std::size_t                          filled = 0; // of data, by DATA that came while it waited
    std::error_code                      err;
    bool                                 woken = false; // its reader's been resumed
    bool                                 busy  = false; // another read waits for the stream
    std::coroutine_handle<>              awaiting;
    std::optional<io::clock::time_point> timeout_at; // a tunnel's read timeout's end (see stream_tunnel::check_wait())
};

// stream_reader reads what a stream receives (a request's body, or a response's), as its DATA arrives, giving the
// peer back its windows as it goes. A read that finds something received returns without a coroutine, and one that has
// to wait is an operation (see stream_read). Reads take turns: one that's waiting is the reader's only one
// (std::errc::device_or_resource_busy for another), and its stream's.
class stream_reader final : public io::reader
{
public:
    explicit stream_reader(http2_stream_base* stream) noexcept
        : stream{stream}
        , waiting{stream}
    {}

    coro::task<io::result> read(std::span<std::byte> data) override;
    using io::reader::read;

    [[nodiscard]] io::handle native_handle() const noexcept override;

    // call_at_start sets what's awaited before the first read into a non-empty span reads anything (see
    // io::start_callback): e.g. the server's 100 Continue. It's awaited without the connection's lock.
    void call_at_start(io::start_callback callback) noexcept { at_start = callback; }

    // owner is the stream it reads (e.g. for the server to find a request's stream from its body).
    [[nodiscard]] http2_stream_base* owner() const noexcept { return stream; }

private:
    // read_after_start awaits the start callback, then reads.
    coro::task<io::result> read_after_start(std::span<std::byte> data);

    http2_stream_base* stream;
    io::start_callback at_start;
    stream_read        waiting;
};

// stream_writer writes what a stream sends (a response's body, or a request's) as DATA, waiting for the
// flow-control windows to have room, and its turn (see http2_connection_base::step_write()). A write that can go at
// once returns without a coroutine.
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

// stream_tunnel is a stream once it's a tunnel (see http/tunnel.hpp): once a 2xx has answered its extended CONNECT
// (RFC 8441), its DATA both ways is the tunnel's protocol's (e.g. a WebSocket's), read and written through buffers of
// 4 KiB each way over the stream's own reader and writer (so frame headers and small messages are gathered, and large
// payloads go past them, straight to the stream, and from it). Its stream's window is wider than a request's, as a
// socket's buffers are (256 KiB: see the constructor), so a large message goes without waiting for a WINDOW_UPDATE;
// what its stream holds of it meanwhile is what's unread (see received_data), whatever a burst took. A read that waits
// is limited by the tunnel's own timeout and deadline, and canceled by its stop token, failing having read nothing
// while the stream goes on (as a socket's would), where the stream's own timeouts reset it: a read asks check_wait()
// before it waits, and again whenever it's woken (see stream_read): by the connection's timer at wake_at, by a deadline
// set meanwhile, or by stop.
//
// A stream's tunnel is its stream's: made once (holding the connection's lock; its maker then wakes the writer, for the
// window's WINDOW_UPDATE), and destroyed with it, holding the lock too, so its stop token (see set_stop_token()) must
// be removed before then, as its callback takes the lock.
class stream_tunnel final : public tunnel
{
public:
    // closing is the server's close() token (none for a client's).
    stream_tunnel(http2_stream_base& stream, std::stop_token closing);

    stream_tunnel(const stream_tunnel&)            = delete;
    stream_tunnel& operator=(const stream_tunnel&) = delete;
    stream_tunnel(stream_tunnel&&)                 = delete;
    stream_tunnel& operator=(stream_tunnel&&)      = delete;
    ~stream_tunnel()                               = default;

    [[nodiscard]] io::buffered_reader& reader() noexcept override { return in; }
    [[nodiscard]] io::buffered_writer& writer() noexcept override { return out; }

    void set_read_timeout(std::optional<io::clock::duration> timeout) noexcept override;
    void set_read_deadline(std::optional<io::clock::time_point> deadline) noexcept override;

    // set_stop_token registers a callback on stop (removing the last's first, which waits for it if it's being made,
    // so it's called without the connection's lock) that marks the tunnel stopped and wakes the stream's read and
    // waits, which then fail (std::errc::operation_canceled): a read that has to wait, and a write that waits for its
    // turn or the windows (with what it sent before; see http2_connection_base::step_write()).
    void set_stop_token(std::stop_token stop) noexcept override;

    [[nodiscard]] std::stop_token closing() const noexcept override { return closing_token; }
    [[nodiscard]] io::scheduler*  get_scheduler() const noexcept override;

    // abort resets the stream (CANCEL), failing it with std::errc::connection_aborted.
    void abort() noexcept override;

    // end ends this side of the stream (END_STREAM: RFC 8441, section 5, has an orderly TCP close as END_STREAM), after
    // what's been flushed, unless it's ended or failed: nothing's sent after it. Not holding the lock, which it takes.
    void end() noexcept;

    // check_wait is what a read that has nothing to read comes to before it waits, and whenever it's woken (holding the
    // lock): an error if it's not to wait (stopped, or past its deadline or timeout: timeout_at is when its timeout
    // passes, which it sets the first time); none otherwise, having set when the timer's to wake it (wake_at), if ever.
    std::error_code check_wait(std::optional<io::clock::time_point>& timeout_at);

    // Guarded by the connection's lock: when the timer's to wake a read that's waiting (its deadline, or its timeout's
    // end), and whether stop's been requested.
    std::optional<io::clock::time_point> wake_at;
    bool                                 stopped = false;

private:
    // stopper is the stop token's callback.
    struct stopper
    {
        stream_tunnel* tunnel;

        void operator()() const noexcept;
    };

    http2_stream_base& stream;
    std::stop_token    closing_token;

    // Guarded by the connection's lock.
    std::optional<io::clock::time_point> deadline;
    std::optional<io::clock::duration>   timeout;

    stream_reader       raw_in;
    io::buffered_reader in;
    io::buffered_writer out;

    std::optional<std::stop_callback<stopper>> on_stop;
};

// http2_stream_base is what a stream's reader and writer use: what it's received, and how it's ended. Its fields are
// guarded by its connection's lock. It's a scheduler_node (its connection schedules its streams' DATA by priority): its
// priority, and its place among the streams waiting to send.
class http2_stream_base : public scheduler_node
{
public:
    http2_stream_base(http2_connection_base* conn, http2::stream_id id, coro::thread_pool* pool)
        : scheduler_node{id}
        , conn{conn}
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

    received_data received; // DATA not read yet
    bool          received_end = false;

    // The trailers received (those that may be: see trailer_allowed()), held until the reader reaches the end of what
    // was received, which moves them to trailers_to, if it's set (e.g. a request's): so whoever looks at them does on
    // the reader's task, once it's read to the end, never as the connection's reader adds them. Made only for a
    // message that has them, so a stream without any isn't larger for them.
    std::unique_ptr<trailer_fields> received_trailers;
    trailer_fields*                 trailers_to = nullptr;

    // hand_over_trailers moves the trailers received to trailers_to, if both are there (holding the lock).
    void hand_over_trailers() noexcept;

    // The read waiting for what's to be received, if one is (see stream_read), and wake_reader, which finishes it if
    // there's something for it (holding the lock): called whenever the stream receives, ends or fails, and when a
    // tunnel's read's stop or time may have come.
    stream_read* reading = nullptr;
    void         wake_reader() noexcept;

    bool            ended = false; // this side has sent END_STREAM
    std::error_code failure;       // reset, timed out, or the connection ended (see fail_stream())

    // What's left to send of a body whose length is known, if its last DATA frame ends the stream (rather than an empty
    // DATA frame after it, 9 bytes more): set as the head's sent (a response's Content-Length, or a request's), unless
    // trailers are to follow. The frame that brings it to 0 carries END_STREAM, and the stream's then ended. None:
    // something else ends it (the head, trailers, or an empty DATA frame).
    std::optional<std::size_t> data_left;

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

    // What a write waiting in the connection's scheduler has left to send (the writer's span, valid while its write
    // is awaited), and how much of it the connection's writer has framed: the writer frames it, a frame at a time,
    // while the stream's queued, and takes it out once it's all framed (or the stream fails, which takes it out first),
    // so it never touches the span once the write has returned.
    std::span<const std::byte> sending;
    std::size_t                sending_done = 0;

    // The stream's tunnel, once it's one (see stream_tunnel): its reads' limits and stop, which stream_reader checks.
    std::unique_ptr<stream_tunnel> tunnel;

    // notified whenever any of the above changes (but what's received: its reader's woken by wake_reader()), or the
    // stream's (or connection's) window grows.
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
    request,  // a request's head: without Host, which :authority replaces, and TE unless it's "trailers" (kept even if
              // Connection names it, as HTTP/1.1 has it do)
    trailers, // either's trailers: without the fields that can't be trailers (see trailer_allowed())
};

// append_fields appends from's fields to fields as HTTP/2 sends them: names lowercased (into names, which fields views,
// so names mustn't be touched until fields are sent), values without the whitespace around them (see trim_value),
// without connection-specific fields (nor those Connection names; nor what section leaves out), and with credentials
// never indexed. It fails with std::errc::invalid_argument, appending nothing, if a field's name or value isn't valid
// (see valid_fields), as HTTP/1.1's encoder does: any field's, even one it would leave out. from is headers, or
// trailer_fields (it's defined for those only).
template<std::size_t N>
[[nodiscard]] std::error_code append_fields(const basic_headers<N>&    from,
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
    // waits_writable is whether the writer waits for the connection to be writable before each round (see
    // write_loop()): with TCP_NOTSENT_LOWAT set on it. writable, if set, is what it waits with instead (for tests).
    http2_connection_base(io::scheduler*               scheduler,
                          connection&                  conn,
                          http2::role                  side,
                          const http2::session_config& config,
                          spdlog::logger&              logger,
                          bool                         waits_writable = false,
                          writable_function            writable       = {});

    http2_connection_base(const http2_connection_base&)            = delete;
    http2_connection_base& operator=(const http2_connection_base&) = delete;
    http2_connection_base(http2_connection_base&&)                 = delete;
    http2_connection_base& operator=(http2_connection_base&&)      = delete;
    ~http2_connection_base() override                              = default;

    // Used by the streams, holding the lock.
    connection_mutex mu;
    http2::session   session;
    chunk_pool       chunks; // for what the streams receive (see received_data)

    // The streams waiting to send DATA, in their turns by priority (see step_write()), guarded by mu. Each side sets
    // its streams' priorities through it (update()), so a waiting stream moves when its priority changes: a server's
    // from the client's signals, a client's from its request's Priority field, then client_response::set_priority().
    http2_scheduler send_order;

    // write_state is how far a stream's write has gone, from one step to the next.
    struct write_state
    {
        std::size_t written = 0;     // what the write submitted itself
        bool        queued  = false; // it's handed the rest to the scheduler
    };

    // write_step is what a step of a write came to: its result, once it's done; otherwise what it's to wait for before
    // the next step (a notifier, and the value it read).
    struct write_step
    {
        bool            done = false;
        io::result      result;
        coro::notifier* wait = nullptr;
        std::uint64_t   seen = 0;
    };

    // step_write takes a step of a stream's write of data (taking the lock), as far as it can go without waiting.
    //
    // A write (a server's response, or a client's request body) is submitted to the session at once, a frame at a time,
    // while less than max_ahead of DATA waits for the writer (or is being written), no stream waits in the scheduler,
    // and the output's under max_output: so a connection whose streams don't contend for it pays nothing for the
    // scheduler. Otherwise (or for what doesn't fit, or has no window), the stream waits in the scheduler with the
    // rest, for the writer to frame it in its turn (see fill()), and the write returns once it's all framed: so the
    // stream's end (an empty DATA, or trailers) still follows its data, and only max_ahead of DATA (and what's in the
    // socket's buffers) is past reordering.
    //
    // Either way, a body of known length ends with its last DATA frame (see http2_stream_base::data_left).
    write_step step_write(http2_stream_base& s, std::span<const std::byte> data, write_state& state);

    // wake_writer tells the writer there's output, if there is: called without the lock.
    void wake_writer();

    // arm_deadline sets one of a stream's deadlines for a wait, timeout from now, unless it's set already (holding the
    // lock): telling the timer, if it's sooner than it knew.
    void arm_deadline(std::optional<io::clock::time_point>& deadline, std::chrono::milliseconds timeout);

    // wake_timer_at tells the timer of a time it's to act at (holding the lock): e.g. a tunnel's read's wake_at.
    void wake_timer_at(io::clock::time_point at);

    // trim_later has the timer trim chunks (see chunk_pool::trim()) while it keeps some (holding the lock): called once
    // chunks have been given back to it.
    void trim_later();

    // get_logger is the connection's logger (e.g. for what its streams' heads and ends drop).
    [[nodiscard]] spdlog::logger& get_logger() const noexcept { return logger; }

    // get_scheduler is the scheduler the connection waits on (e.g. for its tunnels' users).
    [[nodiscard]] io::scheduler* get_scheduler() const noexcept { return scheduler; }

    // fail_stream fails s with err, unless it has failed already (holding the lock), queuing its stop source (if it
    // has one, and it isn't a tunnel, whose user finds its failure in its reads and writes) for stop_pending() to stop.
    // It finishes a read that's waiting; waking its other waits is the caller's.
    //
    // Stop sources are never stopped holding the lock: std::stop_source::request_stop() runs the callbacks registered
    // on it inline, and they're anyone's (the handler's, a scheduler wait's, ...), taking locks of their own. So each
    // place that may fail a stream calls stop_pending() once it's released the lock: the reader after receiving, the
    // timer after its checks, the writer and run() after end(), and a stream's own failed write. A copy of the source
    // is queued, rather than the stream, as the stream may be gone by then.
    void fail_stream(http2_stream_base& s, std::error_code err);

    // stop_pending stops the sources fail_stream() queued (without the lock).
    void stop_pending();

    // notify_writer tells the writer there's output (holding the lock, or not): e.g. a read's WINDOW_UPDATE.
    void notify_writer() noexcept { writer_wakeup.notify(); }

    [[nodiscard]] io::handle native_handle() const noexcept { return detail::native_handle(conn); }

    // unqueue takes a stream out of the scheduler (taking the lock): for a write destroyed while it waits there, whose
    // span the writer mustn't touch once it's gone.
    void unqueue(http2_stream_base& s);

    // wait_queued waits until at least n streams wait in the scheduler (true), or the connection's ending (false): for
    // tests, to know that streams have queued (e.g. with no window to send) before letting them send.
    coro::task<bool> wait_queued(std::size_t n);

protected:
    // run runs the connection until it ends (when the peer closes it, or it fails, or end() is called): pending is what
    // was read of it before. Once it's ending, the writer writes what's left and shuts the connection down
    // (shutdown(): close_notify over TLS, then FIN), so the peer sees it end at once, while the reader goes on reading
    // (and dropping) what the peer still sends, until it closes its side too, or for linger_time; then it waits for the
    // writer and the timer, and only then awaits finish_streams() (e.g. the server's handlers, which may take a while).
    //
    // NOTE: the reader lingers as closing a socket with input unread (or, on macOS, input arriving after shutting down
    //       its reading side) resets the connection, and a reset discards what's still on its way to the peer: e.g.
    //       the end of a response, while the client sends WINDOW_UPDATEs for what it's read of it.
    coro::task<> run(std::vector<std::byte> pending);

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

    // end_received ends what a stream receives (holding the lock): its trailers, or its head, ended it.
    static void end_received(http2_stream_base& s);

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
    bool receive(std::vector<std::byte>& pending);

    // write_loop is the writer: each round, it takes the session's output (see take_output()) and writes it, until the
    // connection's ending and what's left is written; then it shuts the connection down. With waits_writable (with
    // TCP_NOTSENT_LOWAT set), a round after one that wrote first waits for the connection to be writable
    // (wait_for_socket()): what's written but not yet sent under the mark. So fill() frames only when the socket can
    // take more, and meanwhile streams' writes queue (past max_ahead) for their turns: what's past reordering is the
    // mark (in the kernel) and a round (in the writer's hands), not the send buffer. Control frames wait with the DATA:
    // TCP would deliver them after what the kernel holds anyway. A wait that fails ends the connection as a write that
    // fails does (the socket's write deadline, timeout and stop token apply to it as to a write). wrote_at is when the
    // writer's back from a round's write (and its wait), so a stream waiting its turn is put off (see waiting_turn())
    // while the writer's rounds come within its write timeout of each other: one held up longer by the socket fails
    // anyway, its wait timing out.
    coro::task<> write_loop();

    // write_failed ends the connection after the writer's write (or wait) failed with failed, or leaves that to the
    // reader (see its definition: which, and why), waiting until it has.
    coro::task<> write_failed(std::error_code failed);

    // wait_for_socket waits until the connection is writable: with the writable hook, if it's set.
    coro::task<io::result> wait_for_socket();

    coro::task<>                         timer_loop();
    std::optional<io::clock::time_point> check_streams(io::clock::time_point now);

    // trim_chunks trims chunks if it's time (see trim_later()), bringing next forward to the next trim if that's sooner
    // (holding the lock).
    void trim_chunks(io::clock::time_point now, std::optional<io::clock::time_point>& next);

    // fill submits DATA from the streams waiting in the scheduler, a frame at a time in their order, until max_ahead
    // of DATA waits for the writer, or none is ready (has a window); it wakes each stream whose write it's all framed
    // (holding the lock). A stream whose write it's framed keeps its turn for its writer's next write, briefly
    // (turn_held), when the others' would otherwise take it.
    void fill();

    // step_write()'s parts (holding the lock): submit_at_once submits a write as far as it may go at once (failing the
    // stream if it can't submit), and queue_rest queues the rest in the scheduler.
    std::error_code submit_at_once(http2_stream_base& s, std::span<const std::byte> data, write_state& state);
    void            queue_rest(http2_stream_base& s, std::span<const std::byte> data, write_state& state);

    // submit_body submits data of a stream's body to the session (as far as the windows allow; see
    // http2::session::submit_data()), with END_STREAM if it's the last of a body of known length (see
    // http2_stream_base::data_left), marking the stream ended once that's all gone (holding the lock).
    std::expected<std::size_t, std::error_code> submit_body(http2_stream_base& s, std::span<const std::byte> data);

    // take_output is the writer's turn with the lock, after it's written (wrote) or been woken: fill(), then the
    // session's output into out, and the stop sources to stop into stopping. It returns
    // whether the connection's ending.
    bool take_output(std::vector<std::byte>& out, bool wrote, std::vector<std::stop_source>& stopping);

    // hold_turn_of holds the turn of a stream whose write fill() has framed, if the others would take it, returning
    // whether it did; turn_still_held is whether the turn held still is (forgetting it once it's over). Both holding
    // the lock.
    bool hold_turn_of(const http2_stream_base& s);
    bool turn_still_held();

    // waiting_turn is whether a stream's write is held up by the others' turns rather than by the peer: it waits in
    // the scheduler with a window, and the writer's written within its timeout (holding the lock). Its write deadline
    // is then put off (see check_streams()).
    [[nodiscard]] bool waiting_turn(const http2_stream_base& s, io::clock::time_point now) const;

    // Whether the writer waits for the connection to be writable before each round, and with what (see write_loop()).
    const bool        waits_writable;
    writable_function writable;

    // Guarded by mu.
    std::size_t                          data_queued  = 0; // DATA submitted not yet taken
    std::size_t                          data_writing = 0; // of the writer's output, DATA (as data_queued counts it)
    std::optional<io::clock::time_point> wrote_at;         // when the writer was back from writing, streams scheduled
    http2::stream_id                     turn_held = 0;    // the stream whose turn fill() holds, until...
    io::clock::time_point                turn_held_until;  // ...at most this
    bool                                 is_ending   = false;
    bool                                 reader_done = false; // it's stopped lingering
    io::clock::time_point                linger_until;        // when the timer stops the reader, once ending
    std::optional<io::clock::time_point> timer_target;
    std::optional<io::clock::time_point> trim_at; // when the timer next trims chunks, while it keeps some

    coro::notifier             writer_wakeup; // output to write, or the connection's ending
    std::atomic<std::uint64_t> timer_word{0}; // the timer's, for scheduler::wait_on()

    coro::async_scope workers; // the writer and the timer
};

}
