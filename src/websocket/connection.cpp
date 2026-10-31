#include "http/connection.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <compare>
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
#include <type_traits>
#include <utility>
#include <vector>

#include "coro/notifier.hpp"
#include "coro/task.hpp"
#include "http/client.hpp"
#include "http/connection_pool.hpp"
#include "http/headers.hpp"
#include "http/http.hpp"
#include "http/http11.hpp"
#include "http/http2.hpp"
#include "http/http2_client.hpp"
#include "http/http2_config.hpp"
#include "http/request.hpp"
#include "http/response.hpp"
#include "http/tunnel.hpp"
#include "io/buffered_reader.hpp"
#include "io/buffered_writer.hpp"
#include "io/io.hpp"
#include "io/reader.hpp"
#include "io/scheduler.hpp"
#include "io/writer.hpp"
#include "tcp.hpp"
#include "url.hpp"
#include "util/defer.hpp"
#include "util/string_util.hpp"
#include "websocket/frame.hpp"
#include "websocket/handshake.hpp"
#include "websocket/session.hpp"
#include "websocket/websocket.hpp"

namespace
{

using namespace std::string_view_literals;

using net::coro::ready_task;
using net::coro::task;

// transport_failure is what a failed read or write of the transport ends the WebSocket with: the stream ending, however
// it ended (e.g. reset), without a close is abnormal_closure; other errors (e.g. a timeout) are themselves.
std::error_code transport_failure(std::error_code err) noexcept
{
    if (err == net::io::status_condition::closed) return make_error_code(net::websocket::close_code::abnormal_closure);
    return err;
}

std::error_code invalid() noexcept { return std::make_error_code(std::errc::invalid_argument); }

// control_frames are control frames to send (the replies to the peer's ping and close, and the idle timeout's ping),
// encoded: at most a pong, a ping without a payload, and a close.
struct control_frames
{
    std::array<std::byte,
               (2 * (net::websocket::detail::max_header_size + net::websocket::detail::max_control_payload))
                   + net::websocket::detail::max_header_size>
                bytes{};
    std::size_t size = 0;

    void append(const net::websocket::detail::frame_header& header, std::span<const std::byte> payload) noexcept
    {
        size += net::websocket::detail::encode_header(header, std::span{bytes}.subspan(size).first<14>());
        std::ranges::copy(payload, bytes.begin() + static_cast<std::ptrdiff_t>(size));
        if (header.masked)
            net::websocket::detail::apply_mask(std::span{bytes}.subspan(size, payload.size()), header.mask_key, 0);
        size += payload.size();
    }
};

}

namespace net::websocket::detail
{

namespace
{

    // client_transport is what a client's connection is over, which it owns: its tunnel (get()), over a connection of
    // its own (HTTP/1.1) or a stream of the client's (HTTP/2), ended when it's destroyed.
    class client_transport
    {
    public:
        client_transport()                                   = default;
        client_transport(const client_transport&)            = delete;
        client_transport& operator=(const client_transport&) = delete;
        client_transport(client_transport&&)                 = delete;
        client_transport& operator=(client_transport&&)      = delete;
        virtual ~client_transport()                          = default;

        [[nodiscard]] virtual http::detail::tunnel& get() noexcept = 0;

        // close_sent is told once this side's close has gone, after which nothing's sent (perhaps more than once).
        virtual void close_sent() noexcept = 0;
    };

    // client_tunnel is a client's connection (see connect()), as a tunnel: the WebSocket's own, with buffers of its own
    // (connect_options' sizes), whose reader holds what the server sent after its handshake's response.
    class client_tunnel final
        : public http::detail::tunnel
        , public client_transport
    {
    public:
        client_tunnel(std::unique_ptr<http::detail::connection> connection,
                      std::size_t                               read_buffer_size,
                      std::size_t                               write_buffer_size)
            : conn{std::move(connection)}
            , in{&http::detail::as_reader(*conn), read_buffer_size}
            , out{&http::detail::as_writer(*conn), write_buffer_size}
        {}

        client_tunnel(const client_tunnel&)            = delete;
        client_tunnel& operator=(const client_tunnel&) = delete;
        client_tunnel(client_tunnel&&)                 = delete;
        client_tunnel& operator=(client_tunnel&&)      = delete;
        ~client_tunnel() override                      = default;

        [[nodiscard]] io::buffered_reader& reader() noexcept override { return in; }
        [[nodiscard]] io::buffered_writer& writer() noexcept override { return out; }

        void set_read_timeout(std::optional<io::clock::duration> timeout) noexcept override
        {
            socket().set_read_timeout(timeout);
        }

        void set_read_deadline(std::optional<io::clock::time_point> deadline) noexcept override
        {
            socket().set_read_deadline(deadline);
        }

        // NOTE: over TLS, the socket's token is the stream's (see tls::stream::set_stop_token()).
        void set_stop_token(std::stop_token stop) noexcept override { socket().set_stop_token(std::move(stop)); }

        // a client has no server closing around it.
        [[nodiscard]] std::stop_token closing() const noexcept override { return {}; }

        [[nodiscard]] io::scheduler* get_scheduler() const noexcept override
        {
            return http::detail::socket_of(*conn).get_scheduler();
        }

        // NOTE: what's written but not flushed is dropped with the tunnel, which ends the connection.
        void abort() noexcept override { socket().shutdown_read(); }

        // socket is the TCP connection (under TLS's, if it's TLS), for its timeouts.
        [[nodiscard]] tcp_socket& socket() noexcept { return http::detail::socket_of(*conn); }

        [[nodiscard]] http::detail::tunnel& get() noexcept override { return *this; }

        // nothing: RFC 6455 (section 7.1.1) has the server end TCP first, which the client waits for (see await_end()).
        void close_sent() noexcept override {}

    private:
        std::unique_ptr<http::detail::connection> conn;
        io::buffered_reader                       in;
        io::buffered_writer                       out;
    };

    // stream_transport is a client's stream of an HTTP/2 connection of the client's, which an extended CONNECT opened
    // (RFC 8441), as a tunnel (see http::detail::stream_tunnel): the connection goes on with the client's requests
    // beside it, and holding the stream keeps it open.
    class stream_transport final : public client_transport
    {
    public:
        explicit stream_transport(http::detail::http2_tunnel&& t) noexcept
            : held{std::move(t)}
        {}

        [[nodiscard]] http::detail::tunnel& get() noexcept override { return held.get(); }

        // this side of the stream ends (END_STREAM), as RFC 8441 (section 5) has an orderly TCP close: with nothing to
        // wait out (no TIME_WAIT, as TCP's has for whoever closes first), at once, so the server's stream ends cleanly
        // once the server's done (its handler returns, the close handshake done), rather than being reset for a side
        // the client left open. The client waits for the server's end all the same (see await_end()).
        void close_sent() noexcept override { held.get().end(); }

    private:
        http::detail::http2_tunnel held;
    };

}

// connection_state is a connection's: the session, and what the tasks reading and writing share, in one allocation
// (so that it doesn't move, as waiters and the message's reader and writer point to it).
//
// Reading: one task at a time (reading, a claim), which owns the read side's state (from current to scratch) while it
// holds the claim. It reads frames from the tunnel's buffered reader (pump(), which takes what's buffered without a
// coroutine, so a message that's there already is read without one), answering the peer's pings and close as it comes
// to them (send_replies()).
//
// Writing: the tunnel's buffered writer is written by one task at a time, which holds the wire (wire_busy); and a
// message written a frame at a time (begin_message()) holds the message turn (message_open) until it ends, which whole
// messages wait for too, but control frames don't. Whoever waits for either waits on turn. A read's replies don't wait:
// if the wire's busy, they're left pending, for its holder to send before it lets go (write_replies_or_release()).
//
// Timeouts and stopping cost reads next to nothing until they come: the idle timeout is half of it as the transport's
// read timeout, whose expiry a read takes for the time to ping (wait_failed()); the server's close() (interrupt) sets a
// flag that each step of a read (pump()) looks at, so a peer that never lets a read wait can't keep the WebSocket from
// going away, and a read deadline of now, which a read that's waiting finds the same way as the idle timeout,
// interrupting only reads (the transport's stop token would cancel writes too, cutting a frame short); and stop is the
// transport's stop token, canceling its waits, with a callback (cancel()) for the rest.
struct connection_state
{
    // A server's state is over its connection's tunnel (t), a client's over its own (own, which it keeps: t is its).
    connection_state(http::detail::tunnel&               t,
                     std::unique_ptr<client_transport>&& own,
                     role                                r,
                     const connection_options&           options,
                     std::string_view                    protocol,
                     std::stop_token                     stop)
        : owned{std::move(own)}
        , tunnel{t}
        , scheduler{tunnel.get_scheduler()}
        , side{r}
        , close_timeout{options.close_timeout}
        , idle_wait{half_of(options.idle_timeout)}
        , subprotocol{protocol}
        , sess{r, limits{.max_message = options.max_message}}
        , turn{scheduler->pool()}
        , closing{tunnel.closing()}
    {
        tunnel.set_read_timeout(idle_wait);

        // NOTE: last, as they may run at once (if stop's been requested already), and use the rest.
        if (stop.stop_possible())
        {
            tunnel.set_stop_token(stop);
            on_stop.emplace(std::move(stop), canceler{this});
        }
        if (closing.stop_possible()) on_closing.emplace(closing, interrupt{this});
    }

    connection_state(const connection_state&)            = delete;
    connection_state& operator=(const connection_state&) = delete;
    connection_state(connection_state&&)                 = delete;
    connection_state& operator=(connection_state&&)      = delete;

    // the tunnel's left as it was found, for the server's end of the connection.
    ~connection_state()
    {
        // NOTE: the callbacks first, waiting for one being made, as they use the tunnel.
        on_stop.reset();
        on_closing.reset();

        tunnel.set_stop_token({});
        tunnel.set_read_timeout(std::nullopt);
        tunnel.set_read_deadline(std::nullopt);
    }

    // open makes a client's connection, over its own transport, once the handshake is done.
    static connection open(std::unique_ptr<client_transport> own,
                           const connection_options&         options,
                           std::string_view                  protocol,
                           std::stop_token                   stop)
    {
        auto& t = own->get();
        return connection{
            std::make_unique<connection_state>(t, std::move(own), role::client, options, protocol, std::move(stop))};
    }

    // half_of is half of an idle timeout, as a read's timeout; none, if it's 0.
    static std::optional<io::clock::duration> half_of(std::chrono::milliseconds idle_timeout) noexcept
    {
        if (idle_timeout <= std::chrono::milliseconds::zero()) return std::nullopt;
        return std::chrono::duration_cast<io::clock::duration>(idle_timeout) / 2;
    }

    // interrupt is the server's close()'s callback: the next step of a read goes away (see pump() and go_away()), and
    // a read that's waiting waits no more, failing with std::errc::timed_out having read nothing.
    struct interrupt
    {
        connection_state* state;

        void operator()() const noexcept
        {
            // NOTE: relaxed, as the flag says nothing of other memory: take_closing() unregisters this callback, which
            //       waits for it to return, before it touches the deadline. Set before the deadline, so a read that the
            //       deadline wakes sees it (the deadline's set under the transport's lock, the socket's or the HTTP/2
            //       connection's, which the wake goes through).
            state->server_closing.store(true, std::memory_order::relaxed);
            state->tunnel.set_read_deadline(io::clock::now());
        }
    };

    // canceler is stop's callback.
    struct canceler
    {
        connection_state* state;

        void operator()() const noexcept { state->cancel(); }
    };

    // message_reader is the connection's message(): the message next_message() began.
    class message_reader final : public io::reader
    {
    public:
        explicit message_reader(connection_state* state) noexcept
            : state{state}
        {}

        coro::task<io::result> read(std::span<std::byte> data) override;

        using io::reader::read;

        [[nodiscard]] io::handle native_handle() const noexcept override;

    private:
        connection_state* state;
    };

    // What a read is for.
    enum class goal : std::uint8_t
    {
        begin, // next_message(): a message's first frame begun (what's left of one before it skipped)
        whole, // read_message(): a whole message, into the target's buffer
        some,  // message().read(): some of the current message's data, into the target's span, or its end
        close, // close(): the peer's close, dropping all else
    };

    // What a step of reading (pump()) came to.
    enum class pumped : std::uint8_t
    {
        done,    // the goal's reached
        more,    // more must be read
        reply,   // a reply (a pong) is pending, to send before going on
        failed,  // the connection has failed (failed): perhaps with a close to send
        closing, // the server is closing (see go_away())
    };

    enum class frame_kind : std::uint8_t
    {
        none, // the next frame's header is to be read
        data,
        control,
        dropped,
    };

    // target is where a read puts a message's data.
    struct target
    {
        // whole: grow makes room for size more bytes at the end of buffer (a std::string or std::vector<std::byte>),
        // returning them.
        std::span<std::byte> (*grow)(void* buffer, std::size_t size) = nullptr;
        void* buffer                                                 = nullptr;

        // some: the caller's span, and how much of it has been filled.
        std::span<std::byte> span{};
        std::size_t          filled = 0;
    };

    // ---- reading

    // begin_read claims the read side for a read, unless another has it (std::errc::device_or_resource_busy), or the
    // connection has failed.
    std::error_code begin_read() noexcept
    {
        if (reading.exchange(true, std::memory_order::acq_rel))
            return std::make_error_code(std::errc::device_or_resource_busy);

        if (auto err = failure())
        {
            end_read();
            return err;
        }
        return {};
    }

    // end_read lets the read side go, telling close() if it's waiting for it.
    void end_read() noexcept
    {
        // NOTE: sequentially consistent, as close() sets close_waiting before trying to claim the read side: so either
        //       it claims it, or this sees it waiting.
        reading.store(false, std::memory_order::seq_cst);
        if (close_waiting.load(std::memory_order::seq_cst))
        {
            read_changes.fetch_add(1, std::memory_order::seq_cst);
            scheduler->notify_all(read_changes);
        }
    }

    // pump reads frames from what's buffered toward g, as far as it can without reading more.
    // NOTE: one loop over the frames, whatever the goal, so a message that's all there is read without a coroutine.
    // NOLINTNEXTLINE(readability-function-cognitive-complexity)
    pumped pump(goal g, target& t)
    {
        auto& reader = tunnel.reader();

        // more is what to say when more must be read: unless some data's been read already, which a read returns.
        const auto more = [&] { return g == goal::some && t.filled > 0 ? pumped::done : pumped::more; };

        if (g == goal::some && (!in_message || skipping)) return pumped::done;

        // the server's close() is looked for at each step, rather than only when a read has to wait, so a peer that
        // always leaves something to read (sending nonstop) can't keep the WebSocket open past it: a step takes no more
        // than one read of the transport gave, so it's seen within that. Messages not yet read (buffered ones too) are
        // dropped then, as the ones that come after it are.
        if (closing_now()) return pumped::closing;

        while (true)
        {
            switch (current)
            {
            case frame_kind::none:
            {
                auto begun = begin_frame();
                if (!begun) return fail_reading(begun.error());
                if (!*begun) return more();

                if (current == frame_kind::data && !skipping)
                {
                    if (g == goal::begin && first) return pumped::done;
                    if (g == goal::whole)
                    {
                        dest        = t.grow(t.buffer, static_cast<std::size_t>(sess.remaining()));
                        dest_filled = 0;
                    }
                }
                continue;
            }

            case frame_kind::control:
            case frame_kind::dropped:
            {
                auto skipped = skip_payload();
                if (!skipped) return fail_reading(skipped.error());
                if (!*skipped) return more();

                const auto step = end_control();
                if (step) return *step;
                continue;
            }

            case frame_kind::data: break;
            }

            // a data frame: the message's, or one to skip.
            if (skipping || g == goal::close)
            {
                auto skipped = skip_payload();
                if (!skipped) return fail_reading(skipped.error());
                if (!*skipped) return more();
            }
            else if (g == goal::begin)
            {
                return pumped::done;
            }
            else if (sess.remaining() > 0)
            {
                auto       into = g == goal::whole ? dest.subspan(dest_filled) : t.span.subspan(t.filled);
                const auto size = std::min({static_cast<std::size_t>(sess.remaining()), reader.size(), into.size()});
                if (size == 0) return more();

                into = into.first(size);
                std::ranges::copy(reader.buffered().first(size), into.begin());
                reader.consume(size);
                if (auto err = payload(into)) return fail_reading(err);

                if (g == goal::whole) dest_filled += size;
                else t.filled += size;

                if (sess.remaining() > 0)
                {
                    if (g == goal::some && t.filled == t.span.size()) return pumped::done;
                    continue;
                }
            }

            // the frame's payload is all read.
            auto frame_end = end_frame();
            if (!frame_end) return fail_reading(frame_end.error());

            current = frame_kind::none;
            if (frame_end->what != received::kind::message_end) continue;

            in_message = false;
            if (skipping)
            {
                skipping = false;
                continue;
            }
            if (g == goal::whole || g == goal::some) return pumped::done;
        }
    }

    // begin_frame begins the frame whose header is buffered, returning false if it isn't all there yet.
    std::expected<bool, std::error_code> begin_frame()
    {
        auto& reader  = tunnel.reader();
        auto  decoded = decode_header(reader.buffered());
        if (!decoded) return std::unexpected(decoded.error());
        if (!*decoded) return false;

        std::expected<frame_info, std::error_code> info;
        {
            std::scoped_lock lock{mu};
            info = sess.begin_frame((*decoded)->header);
        }
        if (!info) return std::unexpected(info.error());
        reader.consume((*decoded)->size);

        switch (info->what)
        {
        case frame_info::kind::data:
            current = frame_kind::data;
            first   = info->first;
            if (info->first)
            {
                in_message = true;
                type       = static_cast<message_type>(info->type);
            }
            break;
        case frame_info::kind::control: current = frame_kind::control; break;
        case frame_info::kind::dropped: current = frame_kind::dropped; break;
        }
        return true;
    }

    // skip_payload passes what's buffered of the current frame's payload to the session without keeping it (through
    // scratch, as the session unmasks in place), returning whether it's all been.
    std::expected<bool, std::error_code> skip_payload()
    {
        auto& reader = tunnel.reader();
        while (sess.remaining() > 0 && reader.size() > 0)
        {
            const auto size = std::min({static_cast<std::size_t>(sess.remaining()), reader.size(), scratch.size()});
            const auto into = std::span{scratch}.first(size);
            std::ranges::copy(reader.buffered().first(size), into.begin());
            reader.consume(size);
            if (auto err = payload(into)) return std::unexpected(err);
        }
        return sess.remaining() == 0;
    }

    std::error_code payload(std::span<std::byte> bytes)
    {
        std::scoped_lock lock{mu};
        return sess.payload(bytes);
    }

    std::expected<received, std::error_code> end_frame()
    {
        std::scoped_lock lock{mu};
        return sess.end_frame();
    }

    // end_control ends a control (or dropped) frame: a ping's pong is left pending, and the peer's close answered (left
    // pending too) and reported. It returns what to say, if the read can't go on.
    std::optional<pumped> end_control()
    {
        current = frame_kind::none;
        {
            std::scoped_lock lock{mu};
            auto             frame_end = sess.end_frame();
            if (!frame_end)
            {
                set_failure(frame_end.error());
            }
            else if (frame_end->what == received::kind::ping)
            {
                // only the latest ping is answered, if they come faster than pongs go (RFC 6455, section 5.5.3).
                pong_pending = true;
                pong_size    = static_cast<std::uint8_t>(frame_end->payload.size());
                std::ranges::copy(frame_end->payload, pong.begin());
                return pumped::reply;
            }
            else if (frame_end->what == received::kind::close)
            {
                reason_size = frame_end->close.reason.size();
                std::ranges::copy(frame_end->close.reason, reason.begin());

                // answered with the peer's code, unless this side's close went first.
                if (sess.state() == close_state::close_received)
                {
                    close_pending = true;
                    close_size    = static_cast<std::uint8_t>(sess.close_reply(std::span{close_payload}.first<2>()));
                }
                if (!failed) failed = make_error_code(frame_end->close.code);
            }
            else
            {
                return std::nullopt;
            }
        }

        // writers waiting see the failure.
        turn.notify();
        return pumped::failed;
    }

    // fail_reading fails the connection with what was wrong with what the peer sent: the close for this side to send
    // (left pending).
    pumped fail_reading(std::error_code err)
    {
        {
            std::scoped_lock lock{mu};
            set_failure(err);
        }
        turn.notify();
        return pumped::failed;
    }

    // set_failure fails the connection with err, unless it has failed already, leaving the close it calls for pending.
    // Must be called holding mu.
    void set_failure(std::error_code err) noexcept
    {
        if (failed) return;
        failed = err;

        const auto code = close_code_of(err);
        if (!code || from_peer(err) || sess.state() != close_state::open) return;

        if (auto size = encode_close(*code, {}, close_payload); size.has_value())
        {
            close_pending = true;
            close_size    = *size;
        }
    }

    // fail_transport fails the connection with what failed the transport (see transport_failure()), aborting it,
    // returning the connection's failure (which may have come first).
    std::error_code fail_transport(std::error_code err) noexcept
    {
        std::error_code why;
        {
            std::scoped_lock lock{mu};
            if (!failed) failed = transport_failure(err);
            why = failed;
        }

        turn.notify();
        tunnel.abort();
        return why;
    }

    std::error_code failure() noexcept
    {
        std::scoped_lock lock{mu};
        return failed;
    }

    // cancel fails the connection with std::errc::operation_canceled, as stop has been requested (unless it has failed
    // already), waking whoever waits for a turn, and aborts it. The transport's waits are canceled by its own stop
    // token (so a read ends, letting the read side go, for close() if it's waiting for that).
    void cancel() noexcept
    {
        {
            std::scoped_lock lock{mu};
            if (failed) return;
            failed = std::make_error_code(std::errc::operation_canceled);
        }

        turn.notify();
        tunnel.abort();
    }

    // closing_now is whether the server's close() has come, and no read has taken it yet (see take_closing()).
    // NOTE: a flag of the connection's own, rather than closing.stop_requested(), whose word every connection's
    //       callbacks on the server's token write as they're registered and unregistered; and relaxed (see interrupt),
    //       a plain load, so a read pays no more than that for it.
    [[nodiscard]] bool closing_now() const noexcept { return server_closing.load(std::memory_order::relaxed); }

    // take_closing is whether the server's close() has come (interrupt): if so, it takes the read deadline off, once
    // the callback can't set it any more, so reads wait again, and clears the flag. Only the read side's holder calls
    // it.
    bool take_closing() noexcept
    {
        if (!closing_now()) return false;

        // NOTE: unregistered (waiting for it, if it's being made on another thread) before the deadline's taken off, so
        //       it can't set it again after; and before the flag's cleared, which it then can't set again either.
        on_closing.reset();
        tunnel.set_read_deadline(std::nullopt);
        server_closing.store(false, std::memory_order::relaxed);
        return true;
    }

    // go_away is what the server's close() comes to in a read (holding the read side): it sends this side's close
    // (going away) and reads for the peer's (see read_close()), returning the connection's failure; or, if a close of
    // this side's is under way (close()'s), which goes on as it was, none, for the read to go on.
    coro::task<std::error_code> go_away()
    {
        (void)take_closing();

        bool going_away = false;
        {
            std::scoped_lock lock{mu};
            if (failed) co_return failed;

            if (sess.state() == close_state::open)
            {
                set_failure(sent(close_code::going_away));
                going_away = true;
            }
        }
        if (!going_away) co_return std::error_code{};

        // writers waiting see the failure; the close goes as a reply (left to a write in progress, if there is one),
        // and then the peer's is read for, as close() does.
        turn.notify();
        (void)co_await send_replies();
        co_return co_await read_close(io::clock::now() + close_timeout);
    }

    // wait_failed is what a read's wait failing with err, having read nothing, comes to (holding the read side): the
    // server's close() goes on to the read's next step, which goes away; the first half of the idle timeout passing
    // pings the peer, and the second, with nothing received since, fails the connection; anything else fails it too.
    // It returns the error the read ends with, or none, for it to go on.
    coro::task<std::error_code> wait_failed(std::error_code err)
    {
        if (err != std::errc::timed_out) co_return fail_transport(err);
        if (closing_now()) co_return std::error_code{};

        if (!idle_wait || std::exchange(idle_pinged, true)) co_return fail_transport(err);

        {
            std::scoped_lock lock{mu};
            ping_pending = true;
        }
        co_return co_await send_replies();
    }

    // drive finishes a read (holding the read side, which it lets go): it reads toward g until it's reached (no error)
    // or the connection fails (its failure), reading more and sending replies as pump() asks, starting from what it
    // last said; then it returns what finish makes of that, and the target. One coroutine, for a read that has to wait.
    // NOTE: its reads inline, rather than in helpers, which would be coroutines of their own (a frame each).
    template<typename Finish>
    // NOLINTNEXTLINE(readability-function-cognitive-complexity)
    auto drive(goal g, target t, pumped step, Finish finish)
        -> coro::task<std::invoke_result_t<Finish, std::error_code, const target&>>
    {
        util::defer done{[this] noexcept { end_read(); }};
        auto&       reader = tunnel.reader();

        std::error_code err;
        while (step != pumped::done)
        {
            if (step == pumped::failed)
            {
                err = co_await end_failed(io::clock::now() + close_timeout);
                break;
            }

            if (step == pumped::reply)
            {
                err = co_await send_replies();
                if (err) break;
            }
            else if (step == pumped::closing)
            {
                err = co_await go_away();
                if (err) break;
            }
            else if (ended)
            {
                err = fail_transport(ended);
                break;
            }
            else if (g == goal::whole && current == frame_kind::data && !skipping && reader.size() == 0
                     && sess.remaining() >= reader.capacity())
            {
                // a large frame of a whole message, with nothing buffered: read straight into the message.
                const auto into = dest.subspan(dest_filled, static_cast<std::size_t>(sess.remaining()));
                auto       read = co_await reader.read(into);
                if (read.count > 0)
                {
                    idle_pinged = false;
                    if (auto bad = payload(into.first(read.count)))
                    {
                        step = fail_reading(bad);
                        continue;
                    }
                    dest_filled += read.count;
                }

                if (read.err == std::errc::timed_out)
                {
                    err = co_await wait_failed(read.err);
                    if (err) break;
                }
                else if (read.err)
                {
                    ended = read.err;
                }
            }
            else
            {
                const auto before      = reader.size();
                const auto failed_fill = co_await reader.fill();
                if (reader.size() != before) idle_pinged = false;

                // what was read with the failure is read first.
                if (failed_fill && reader.size() == before)
                {
                    err = co_await wait_failed(failed_fill);
                    if (err) break;
                }
                else if (failed_fill)
                {
                    ended = failed_fill;
                }
            }

            step = pump(g, t);
        }

        co_return finish(err, t);
    }

    // ---- writing

    // What a frame being written is, for which turn it waits.
    enum class frame_use : std::uint8_t
    {
        message,     // a whole message: waits for the wire, and for a message being written a frame at a time
        part,        // a frame of the message being written a frame at a time (its holder's)
        message_end, // that message's last
        control,     // a control frame: waits only for the wire
    };

    // started is what start_frame() came to: a failure, or that it must wait, or the header of the frame to write, the
    // wire being this write's.
    struct started
    {
        std::error_code err{};
        bool            wait = false;
        frame_header    header{};
    };

    // start_frame takes the wire for a frame of use, if it's free (and, for a whole message, no message is being
    // written a frame at a time), and frames it. A streamed message's frame is message's (its writer's).
    started start_frame(frame_use use, opcode op, std::size_t length, std::uint64_t message = 0)
    {
        started start;
        bool    ended = false; // a message written a frame at a time ended, with its last frame refused
        {
            std::scoped_lock lock{mu};

            const bool parted = use == frame_use::part || use == frame_use::message_end;
            if (parted && (!message_open || message != message_count)) return {.err = invalid()};

            if (failed || sess.state() == close_state::close_sent || sess.state() == close_state::closed)
            {
                start.err = failed ? failed : std::make_error_code(std::errc::broken_pipe);
            }
            else if (wire_busy || (use == frame_use::message && message_open))
            {
                return {.wait = true};
            }
            else
            {
                if (parted) op = std::exchange(message_first, false) ? message_op : opcode::continuation;
                auto header = sess.frame_for(op, use != frame_use::part, length);
                if (header) wire_busy = true;
                start.err    = header ? std::error_code{} : header.error();
                start.header = header.value_or(frame_header{});
            }

            // the message's turn goes with its last frame (the wire's release, or now, if it's refused, tells
            // whoever waits).
            if (use == frame_use::message_end)
            {
                message_open = false;
                ended        = static_cast<bool>(start.err);
            }
        }

        if (ended) turn.notify();
        return start;
    }

    // write_frame writes a frame of use with payload, once it's its turn, returning once it's gone (with the replies
    // pending by then).
    coro::task<std::error_code>
    write_frame(frame_use use, opcode op, std::span<const std::byte> payload, std::uint64_t message = 0)
    {
        auto start = start_frame(use, op, payload.size(), message);
        if (start.err) return ready_task<std::error_code>(start.err);
        if (start.wait) return write_waiting(use, op, payload, message);
        return send(start.header, payload);
    }

    coro::task<std::error_code>
    write_waiting(frame_use use, opcode op, std::span<const std::byte> payload, std::uint64_t message)
    {
        while (true)
        {
            const auto seen  = turn.value();
            auto       start = start_frame(use, op, payload.size(), message);
            if (start.err) co_return start.err;
            if (!start.wait) co_return co_await send(start.header, payload);

            (void)co_await turn.wait(seen);
        }
    }

    // send writes a frame (holding the wire), flushes it and lets the wire go.
    coro::task<std::error_code> send(const frame_header& header, std::span<const std::byte> payload)
    {
        std::array<std::byte, max_header_size> head{};
        const auto                             size = encode_header(header, head);

        // usually, the frame fits in the writer's buffer: then it needs no coroutine, unless the flush waits. A
        // client's payload is masked as it's copied there, so the caller's isn't changed.
        if (auto room = tunnel.writer().prepare(size + payload.size()); !room.empty())
        {
            std::ranges::copy(std::span{head}.first(size), room.begin());
            std::ranges::copy(payload, room.begin() + static_cast<std::ptrdiff_t>(size));
            if (header.masked) apply_mask(room.subspan(size), header.mask_key, 0);
            return flush_and_release();
        }

        if (header.masked) return send_masked(head, size, header.mask_key, payload);
        return send_through(head, size, payload);
    }

    // send_masked writes a client's frame that doesn't fit in the writer's buffer: its payload masked into the buffer,
    // a buffer's worth at a time, each flushed in turn.
    coro::task<std::error_code> send_masked(std::array<std::byte, max_header_size> head,
                                            std::size_t                            size,
                                            std::array<std::byte, 4>               key,
                                            std::span<const std::byte>             payload)
    {
        auto&      writer = tunnel.writer();
        const auto header = std::span<const std::byte>{head}.first(size);

        // NOTE: the buffer's empty (the wire's this frame's), and it holds a header.
        auto room = writer.prepare(size);
        if (room.empty())
        {
            if (auto res = co_await writer.flush(); res.err) co_return fail_write(res.err);
            room = writer.prepare(size);
        }
        std::ranges::copy(header, room.begin());

        std::size_t offset = 0;
        while (offset < payload.size())
        {
            const auto free = writer.capacity() - writer.size();
            if (free == 0)
            {
                if (auto res = co_await writer.flush(); res.err) co_return fail_write(res.err);
                continue;
            }

            const auto part = std::min(free, payload.size() - offset);
            room            = writer.prepare(part);
            std::ranges::copy(payload.subspan(offset, part), room.begin());
            apply_mask(room, key, offset);
            offset += part;
        }

        co_return co_await flush_and_release();
    }

    coro::task<std::error_code>
    send_through(std::array<std::byte, max_header_size> head, std::size_t size, std::span<const std::byte> payload)
    {
        auto wrote = co_await io::write_all(tunnel.writer(), std::span<const std::byte>{head}.first(size), payload);
        if (wrote.err) co_return fail_write(wrote.err);
        co_return co_await flush_and_release();
    }

    // flush_and_release flushes what's written, then the replies pending by then, until there are none, then lets the
    // wire go.
    coro::task<std::error_code> flush_and_release()
    {
        while (true)
        {
            auto flushing = tunnel.writer().flush();
            if (!flushing.finished_without_coroutine()) return flush_waiting(std::move(flushing));

            if (auto res = std::move(flushing).result(); res.err)
                return ready_task<std::error_code>(fail_write(res.err));
            if (!write_replies_or_release()) return ready_task<std::error_code>();
        }
    }

    coro::task<std::error_code> flush_waiting(coro::task<io::result> flushing)
    {
        if (auto res = co_await std::move(flushing); res.err) co_return fail_write(res.err);
        if (!write_replies_or_release()) co_return std::error_code{};
        co_return co_await flush_and_release();
    }

    // write_replies_or_release writes the replies pending into the writer's buffer (empty, as it's just been flushed),
    // returning true; or, if there are none, lets the wire go.
    bool write_replies_or_release()
    {
        control_frames frames;
        bool           close_gone = false; // this side's close has gone (with what's just been flushed, perhaps)
        {
            std::scoped_lock lock{mu};
            take_replies(frames);
            if (frames.size == 0)
            {
                wire_busy  = false;
                close_gone = sess.state() == close_state::close_sent || sess.state() == close_state::closed;
            }
        }

        // a client's transport is told (each time the wire's let go after it, as nothing's sent then but what's
        // refused).
        if (close_gone && owned != nullptr) owned->close_sent();

        if (frames.size != 0)
        {
            if (auto room = tunnel.writer().prepare(frames.size); !room.empty())
            {
                std::ranges::copy(std::span{frames.bytes}.first(frames.size), room.begin());
                return true;
            }

            // only after a failed write, whose leftovers are in the buffer: the replies can't go.
            std::scoped_lock lock{mu};
            wire_busy = false;
        }

        turn.notify();
        return false;
    }

    // take_replies frames the replies pending into frames (a pong, the idle timeout's ping, then a close): those the
    // session refuses, as this side's close has gone, are dropped. Must be called holding mu.
    void take_replies(control_frames& frames)
    {
        if (std::exchange(pong_pending, false))
        {
            const auto payload = std::span{pong}.first(pong_size);
            if (auto header = sess.frame_for(opcode::pong, true, payload.size())) frames.append(*header, payload);
        }

        if (std::exchange(ping_pending, false))
        {
            if (auto header = sess.frame_for(opcode::ping, true, 0)) frames.append(*header, {});
        }

        if (std::exchange(close_pending, false))
        {
            const auto payload = std::span{close_payload}.first(close_size);
            if (auto header = sess.frame_for(opcode::close, true, payload.size())) frames.append(*header, payload);
        }
    }

    // send_replies sends the replies pending, if the wire's free; otherwise they're left to its holder.
    coro::task<std::error_code> send_replies()
    {
        control_frames frames;
        {
            std::scoped_lock lock{mu};
            if (wire_busy) return ready_task<std::error_code>();

            take_replies(frames);
            if (frames.size == 0) return ready_task<std::error_code>();
            wire_busy = true;
        }

        // the wire's free, so the buffer's empty (unless a write failed).
        if (auto room = tunnel.writer().prepare(frames.size); !room.empty())
            std::ranges::copy(std::span{frames.bytes}.first(frames.size), room.begin());
        return flush_and_release();
    }

    // fail_write fails the connection with what failed a write (holding the wire, which it lets go), returning the
    // connection's failure.
    std::error_code fail_write(std::error_code err) noexcept
    {
        {
            std::scoped_lock lock{mu};
            wire_busy = false;
        }
        return fail_transport(err);
    }

    // ---- closing

    // close sends this side's close (payload), then waits for the peer's until close_timeout passes (see
    // connection::close()).
    coro::task<std::error_code> close(std::array<std::byte, max_control_payload> payload, std::size_t size)
    {
        const auto deadline = io::clock::now() + close_timeout;

        if (auto err = co_await send_close(std::span{payload}.first(size))) co_return err;
        co_return co_await await_close(deadline);
    }

    // send_close sends this side's close, unless it's gone already (or the peer's close is still to be answered, which
    // its reply does), once the wire's free. It returns the connection's failure, unless it's the peer's close.
    coro::task<std::error_code> send_close(std::span<const std::byte> payload)
    {
        while (true)
        {
            const auto                  seen = turn.value();
            std::optional<frame_header> header;
            bool                        replies = false;
            {
                std::scoped_lock lock{mu};
                const bool       peer_closed = sess.state() == close_state::close_received;
                if (sess.state() == close_state::closed || sess.state() == close_state::close_sent) co_return {};
                if (failed && !peer_closed) co_return failed;

                if (!wire_busy && peer_closed)
                {
                    replies = true;
                }
                else if (!wire_busy)
                {
                    auto framed = sess.frame_for(opcode::close, true, payload.size());
                    if (!framed) co_return framed.error();
                    header    = *framed;
                    wire_busy = true;
                }
            }

            if (header) co_return co_await send(*header, payload);
            if (replies)
            {
                if (auto err = co_await send_replies()) co_return err;
                continue;
            }

            (void)co_await turn.wait(seen);
        }
    }

    // await_close waits for the peer's close, once this side's has gone: reading for it, unless another task is
    // reading, which is waited for. If deadline passes first, it aborts the connection, with std::errc::timed_out.
    coro::task<std::error_code> await_close(io::clock::time_point deadline)
    {
        close_waiting.store(true, std::memory_order::seq_cst);
        util::defer not_waiting{[this] noexcept { close_waiting.store(false, std::memory_order::relaxed); }};

        while (true)
        {
            const auto seen = read_changes.load(std::memory_order::seq_cst);
            if (closed())
            {
                // a client's reader that received the peer's close waits for the server's end of the stream (see
                // await_end()), which this waits for too, as long as the reader has the read side.
                // NOTE: sequentially consistent, as below.
                if (side == role::server || !reading.load(std::memory_order::seq_cst)) co_return std::error_code{};
            }
            else
            {
                if (auto err = failure())
                {
                    tunnel.abort();
                    co_return err;
                }

                // NOTE: sequentially consistent, with close_waiting's store and end_read()'s: so either this claims
                //       the read side, or end_read() sees close_waiting.
                if (!reading.exchange(true, std::memory_order::seq_cst))
                {
                    util::defer done{[this] noexcept { end_read(); }};
                    const auto  err = co_await read_close(deadline);
                    if (closed()) co_return std::error_code{};

                    tunnel.abort();
                    co_return err;
                }
            }

            if (auto err = co_await scheduler->wait_on(read_changes, seen, deadline))
            {
                // the handshake's done, and a client's reader is still waiting for the server's end of the stream:
                // the connection ends with this side's close.
                if (closed()) co_return std::error_code{};
                co_return fail_transport(err);
            }
        }
    }

    // closed is whether the close handshake is done.
    bool closed()
    {
        std::scoped_lock lock{mu};
        return sess.state() == close_state::closed;
    }

    // end_failed is what a read that found the connection failed comes to (holding the read side): it sends the close
    // the failure calls for, if it's this side's to send (or the reply to the peer's), and if that ends a client's
    // close handshake, waits for the server's end of the stream (see await_end()) until deadline. It returns the
    // failure.
    coro::task<std::error_code> end_failed(io::clock::time_point deadline)
    {
        (void)co_await send_replies();
        if (side == role::client && closed()) co_await await_end(deadline);
        co_return failure();
    }

    // await_end waits (holding the read side), once a client's close handshake is done, for the server to end the
    // stream, as RFC 6455 (section 7.1.1) has the server close TCP first, until deadline, dropping whatever comes
    // before it (nothing should). Either way, the client then ends the connection itself (it's destroyed).
    coro::task<> await_end(io::clock::time_point deadline)
    {
        auto&       reader = tunnel.reader();
        util::defer idle{[this] noexcept { tunnel.set_read_timeout(idle_wait); }};

        while (!ended)
        {
            reader.consume(reader.size());

            const auto left = deadline - io::clock::now();
            if (left <= io::clock::duration::zero()) co_return;
            tunnel.set_read_timeout(left);

            // NOTE: however it ended (a reset too), or the timeout passing.
            if (auto err = co_await reader.fill()) ended = err;
        }
    }

    // read_close reads (holding the read side) until the peer's close comes, dropping what comes before it, or until
    // deadline passes: then the connection fails with std::errc::timed_out (aborting it). It returns the connection's
    // failure (the peer's close, if it came).
    coro::task<std::error_code> read_close(io::clock::time_point deadline)
    {
        auto&       reader = tunnel.reader();
        util::defer idle{[this] noexcept { tunnel.set_read_timeout(idle_wait); }};

        target t;
        while (true)
        {
            switch (pump(goal::close, t))
            {
            case pumped::done:
            case pumped::failed: co_return co_await end_failed(deadline);

            case pumped::reply:
                if (auto err = co_await send_replies()) co_return err;
                break;

            // the server's close(): this side's close has gone (or is going) already, so it reads on.
            case pumped::closing: (void)take_closing(); break;

            case pumped::more:
            {
                if (ended) co_return fail_transport(ended);

                // each wait for more no longer than what's left.
                const auto left = deadline - io::clock::now();
                if (left <= io::clock::duration::zero())
                    co_return fail_transport(std::make_error_code(std::errc::timed_out));
                tunnel.set_read_timeout(left);

                if (const auto before = reader.size(); auto err = co_await reader.fill())
                {
                    if (reader.size() == before)
                    {
                        // the server's close(): the next step takes it.
                        if (err == std::errc::timed_out && closing_now()) break;
                        co_return fail_transport(err);
                    }
                    ended = err;
                }
                break;
            }
            }
        }
    }

    // NOTE: first, so it's destroyed last (ending a client's connection, or stream), once nothing uses it.
    std::unique_ptr<client_transport> owned; // a client's transport: none for a server's, its connection's or stream's

    http::detail::tunnel&              tunnel;
    io::scheduler*                     scheduler;
    role                               side;
    std::chrono::milliseconds          close_timeout;
    std::optional<io::clock::duration> idle_wait; // half the idle timeout, the reads' timeout; none if it's off
    std::string                        subprotocol;

    // the session, and what both sides share: the write side's turns, the replies pending, and the failure.
    std::mutex      mu;
    session         sess;
    std::error_code failed{}; // see connection: the first failure, which every operation then returns

    bool          wire_busy     = false; // a task is writing to the tunnel's writer
    bool          message_open  = false; // a message is being written a frame at a time
    bool          message_first = false; // and its next frame is its first
    opcode        message_op    = opcode::text;
    std::uint64_t message_count = 0; // of messages written a frame at a time: the current one's, for its writer
    bool          pong_pending  = false;
    bool          ping_pending  = false; // the idle timeout's
    bool          close_pending = false;
    std::uint8_t  pong_size     = 0;
    std::uint8_t  close_size    = 0;

    std::array<std::byte, max_control_payload> pong{};
    std::array<std::byte, max_control_payload> close_payload{};

    // notified when the wire or the message turn is let go, or the connection fails.
    coro::notifier turn;

    // The read side's claim, and close()'s wait for it (see end_read()); and whether the server's close() has come,
    // and no read has taken it yet (see interrupt).
    std::atomic_bool           reading        = false;
    std::atomic_bool           close_waiting  = false;
    std::atomic_bool           server_closing = false;
    std::atomic<std::uint64_t> read_changes   = 0;

    // The read side's (the claim's holder's).
    frame_kind           current    = frame_kind::none;
    bool                 first      = false; // the current data frame is its message's first
    bool                 in_message = false; // a message's first frame has come, and not its last
    bool                 skipping   = false; // what's left of it is to be skipped
    message_type         type       = message_type::text;
    std::span<std::byte> dest{}; // whole: the current frame's place in the message
    std::size_t          dest_filled = 0;
    std::error_code      ended{};             // what failed the transport's last read, which read some first
    bool                 idle_pinged = false; // half the idle timeout passed, and nothing's been received since

    std::array<std::byte, max_control_payload> scratch{};

    // the peer's close's reason, once it has come.
    std::array<char, max_close_reason> reason{};
    std::size_t                        reason_size = 0;

    message_reader stream{this};

    // the server's close(), and stop: their callbacks (see interrupt and cancel()).
    // NOTE: last, so they're destroyed first (the destructor unregisters them before then anyway).
    std::stop_token                              closing;
    std::optional<std::stop_callback<interrupt>> on_closing;
    std::optional<std::stop_callback<canceler>>  on_stop;
};

coro::task<io::result> connection_state::message_reader::read(std::span<std::byte> data)
{
    using goal   = connection_state::goal;
    using pumped = connection_state::pumped;

    if (data.empty()) return ready_task<io::result>();
    if (auto err = state->begin_read()) return ready_task<io::result>(io::result{.err = err});

    // the end of the message, or of a result: the end of the stream if nothing was read.
    const auto result = [](std::size_t count)
    { return count == 0 ? io::result{.err = make_error_code(io::errc::end_of_stream)} : io::result{.count = count}; };

    connection_state::target t{.span = data};
    const auto               step = state->pump(goal::some, t);
    if (step == pumped::done)
    {
        state->end_read();
        return ready_task<io::result>(result(t.filled));
    }

    return state->drive(goal::some,
                        t,
                        step,
                        [result](std::error_code err, const connection_state::target& t)
                        { return err ? io::result{.err = err} : result(t.filled); });
}

io::handle connection_state::message_reader::native_handle() const noexcept
{
    return state->tunnel.reader().native_handle();
}

}

namespace net::websocket
{

namespace
{

    using detail::connection_state;

    // read_whole reads a whole message into buffer (a std::string or std::vector<std::byte>).
    template<typename Buffer>
    task<std::expected<message_type, std::error_code>> read_whole(connection_state* state, Buffer& buffer)
    {
        using goal   = connection_state::goal;
        using pumped = connection_state::pumped;
        using result = std::expected<message_type, std::error_code>;

        if (auto err = state->begin_read()) return ready_task<result>(std::unexpected(err));

        buffer.clear();
        state->skipping = state->in_message;

        connection_state::target t{
            .grow =
                [](void* to, std::size_t size)
            {
                auto&      buf   = *static_cast<Buffer*>(to);
                const auto start = buf.size();
                buf.resize(start + size);
                return std::as_writable_bytes(std::span{buf}).subspan(start);
            },
            .buffer = &buffer,
        };

        const auto step = state->pump(goal::whole, t);
        if (step == pumped::done)
        {
            state->end_read();
            return ready_task<result>(state->type);
        }

        return state->drive(goal::whole,
                            t,
                            step,
                            [state](std::error_code err, const connection_state::target& /*t*/) -> result
                            {
                                if (err) return std::unexpected(err);
                                return state->type;
                            });
    }

    // as_result is a write's result, of size bytes, from what writing them came to.
    task<io::result> as_result(task<std::error_code> writing, std::size_t size)
    {
        if (writing.finished_without_coroutine())
        {
            auto err = std::move(writing).result();
            return ready_task<io::result>(io::result{.count = err ? 0 : size, .err = err});
        }

        return [](task<std::error_code> writing, std::size_t size) -> task<io::result>
        {
            auto err = co_await std::move(writing);
            co_return io::result{.count = err ? 0 : size, .err = err};
        }(std::move(writing), size);
    }

    // max_response_head is the most a handshake's response head may be: as the HTTP client's responses'.
    constexpr std::size_t max_response_head = std::size_t{1'024} * 1'024;

    // limit is a timeout, or none for 0.
    std::optional<io::clock::duration> limit(std::chrono::milliseconds timeout) noexcept
    {
        if (timeout <= std::chrono::milliseconds::zero()) return std::nullopt;
        return timeout;
    }

    // endpoint is what a WebSocket's url says to connect to: the URL as HTTP's (ws as http, and wss as https), and its
    // port (the scheme's default, if it has none).
    struct endpoint
    {
        url         target;
        std::string port;
    };

    std::expected<endpoint, std::error_code> endpoint_of(std::string_view text)
    {
        auto parsed = url::parse(text);
        if (!parsed.has_value()) return std::unexpected(invalid());

        const auto is = [&](std::string_view scheme)
        { return util::equal_ignore_case(std::string_view{parsed->scheme}, scheme); };

        bool tls = false;
        if (is("wss"sv) || is("https"sv)) tls = true;
        else if (!is("ws"sv) && !is("http"sv))
            return std::unexpected(std::make_error_code(std::errc::protocol_not_supported));

        parsed->scheme = tls ? "https" : "http";
        auto port      = !parsed->port.empty() ? parsed->port : std::string{tls ? "443" : "80"};
        return endpoint{.target = std::move(*parsed), .port = std::move(port)};
    }

    // valid_sizes is whether a connection's sizes are in their ranges (see connection_options).
    bool valid_sizes(const connection_options& options) noexcept
    {
        return http::detail::valid_window(options.http2_window)
            && options.http2_buffer_size >= http::detail::min_buffer_size;
    }

    // tunnel_options_of is what an HTTP/2 stream's tunnel is opened with for a connection's options.
    http::detail::tunnel_options tunnel_options_of(const connection_options& options) noexcept
    {
        return {.window = options.http2_window, .buffer_size = options.http2_buffer_size};
    }

    // failure is what opening a WebSocket failed with: once stop's been requested, that, whatever its cancellation
    // caused (e.g. a TLS read cut short).
    std::unexpected<std::error_code> failure(const connect_options& options, std::error_code err)
    {
        if (options.stop.stop_requested()) err = std::make_error_code(std::errc::operation_canceled);
        return std::unexpected(err);
    }

    // open_stream opens a WebSocket over a stream of conn (an HTTP/2 connection of the client's, whose server allows
    // extended CONNECT, with a stream reserved for it), as an extended CONNECT (RFC 8441, section 5): the request's
    // head is written with io_timeout and its response read within response_header_timeout, as over HTTP/1.1, and stop
    // ends both, then the WebSocket. state says how far it got, if it failed.
    task<std::expected<connection, std::error_code>>
    open_stream(std::shared_ptr<http::detail::http2_client_connection> conn,
                const endpoint&                                        where,
                connect_options&                                       options,
                http::detail::http2_client_connection::send_state&     state)
    {
        auto request = detail::make_http2_request(where.target, options);
        request.stop = options.stop;

        auto opened = co_await conn->open_tunnel(request, "websocket"sv, tunnel_options_of(options.connection), &state);
        if (!opened.has_value()) co_return failure(options, opened.error());

        // NOTE: a stream that isn't the WebSocket's (refused here) is done with as the response goes: reset, unless
        //       both sides have ended it. Only a 2xx, which check_response() requires, opened the tunnel.
        const auto chosen = detail::check_response(opened->head, {}, options.subprotocols);
        auto&      tunnel = opened->tunnel;
        if (!chosen.has_value() || !tunnel.has_value())
            co_return failure(options, chosen.error_or(make_error_code(errc::bad_status)));
        if (options.stop.stop_requested()) co_return failure(options, {});

        auto own = std::make_unique<detail::stream_transport>(std::move(*tunnel));
        co_return connection_state::open(std::move(own), options.connection, *chosen, std::move(options.stop));
    }

    // open connects to where, and opens a WebSocket over the connection (see connect()).
    // NOTE: everything by value, as connect() returns its task rather than awaiting it.
    task<std::expected<connection, std::error_code>>
    open(std::shared_ptr<http::detail::connection_pool> pool, endpoint where, connect_options options)
    {
        const auto fail = [&options](std::error_code err) { return failure(options, err); };

        // over HTTP/2, on a connection the client has to the host whose server allows it, with a stream free; or, if
        // there's none (as Chrome does), or the stream's refused before the server's looked at it, or the server asks
        // for HTTP/1.1 (HTTP_1_1_REQUIRED, RFC 9113 section 7) before answering anything, over HTTP/1.1.
        if (options.http2)
        {
            if (auto shared = pool->reserve_tunnel(where.target.scheme, where.target.host, where.port))
            {
                http::detail::http2_client_connection::send_state state;
                auto opened = co_await open_stream(std::move(shared), where, options, state);
                if (opened.has_value()) co_return opened;

                const auto err = opened.error();
                const bool http11 =
                    err == http::http2::error_code::http_1_1_required && (state.unsent || state.http11_required);
                if (err != http::http2::error_code::refused_stream && !http11) co_return opened;
            }
        }

        auto conn = co_await pool->connect_apart(where.target.scheme, where.target.host, where.port, options.stop);
        if (!conn.has_value()) co_return fail(conn.error());

        auto own = std::make_unique<detail::client_tunnel>(std::move(*conn),
                                                           options.read_buffer_size,
                                                           options.write_buffer_size);

        // the handshake's request is written (as the WebSocket's frames are) with io_timeout, and its response read
        // within response_header_timeout, both ended by stop.
        const auto& config = pool->options();
        auto&       sock   = own->socket();
        sock.set_write_timeout(limit(config.io_timeout));
        sock.set_stop_token(options.stop);

        const auto handshake = detail::make_request(where.target, options);
        if (auto head = co_await http::http11::request_encode_head(&own->writer(), handshake.request); !head)
            co_return fail(head.error());
        if (auto flushed = co_await own->writer().flush(); flushed.err) co_return fail(flushed.err);

        if (auto wait = limit(config.response_header_timeout)) sock.set_read_deadline(io::clock::now() + *wait);
        auto response =
            co_await http::http11::response_decode(&own->reader(), http::request_method::GET, max_response_head);
        sock.set_read_deadline(std::nullopt);
        if (!response.has_value()) co_return fail(response.error());

        // NOTE: what follows the head (frames the server sent right after its response, if it's a 101) stays in the
        //       reader, for the WebSocket: the response's body (the rest of the connection, after a 101) is never read.
        const auto chosen = detail::check_response(*response, handshake.key, options.subprotocols);
        if (!chosen.has_value()) co_return fail(chosen.error());
        if (options.stop.stop_requested()) co_return fail({});

        co_return connection_state::open(std::move(own), options.connection, *chosen, std::move(options.stop));
    }

    // refuse responds to a handshake that's refused for err.
    task<> refuse(http::response_writer& rw, std::error_code err)
    {
        auto code = http::status::BAD_REQUEST;
        if (err == errc::origin_refused)
        {
            code = http::status::FORBIDDEN;
        }
        else if (err == errc::bad_version)
        {
            // the version the server speaks (RFC 6455, section 4.2.2), and the protocol it's to be upgraded to (RFC
            // 9110, section 15.5.22).
            code = http::status::UPGRADE_REQUIRED;
            rw.headers().set("Sec-WebSocket-Version"sv, "13"sv);
            rw.headers().set("Upgrade"sv, "websocket"sv);
            rw.headers().set("Connection"sv, "Upgrade"sv);
        }

        const auto text = err.message() + "\n";
        rw.headers().set("Content-Type"sv, "text/plain; charset=utf-8"sv);
        auto* body = co_await rw.send(code, text.size());
        (void)co_await io::write_all(*body, text);
    }

}

task<std::expected<connection, std::error_code>>
accept(const http::server_request& req, http::response_writer& rw, const accept_options& options)
{
    // a mistake in the options is the handler's, which is left to respond.
    if (!valid_sizes(options.connection)) co_return std::unexpected(std::make_error_code(std::errc::invalid_argument));

    auto key = detail::check_request(req);
    if (auto refused = key.has_value() ? detail::check_origin(req, options) : key.error())
    {
        co_await refuse(rw, refused);
        co_return std::unexpected(refused);
    }

    const auto subprotocol = detail::select_subprotocol(options.subprotocols, req.headers);

    // over HTTP/2, the stream's 200 says it all (RFC 8441, section 5): no Upgrade, Connection nor accept.
    http::headers fields;
    if (req.version.major != 2)
    {
        fields.set("Upgrade"sv, "websocket"sv);
        fields.set("Connection"sv, "Upgrade"sv);
        fields.set("Sec-WebSocket-Accept"sv, detail::accept_key(*key));
    }
    if (!subprotocol.empty()) fields.set("Sec-WebSocket-Protocol"sv, subprotocol);

    auto switched = co_await http::detail::switch_protocols(rw, fields, tunnel_options_of(options.connection));
    if (!switched.has_value()) co_return std::unexpected(switched.error());

    co_return connection{std::make_unique<connection_state>(**switched,
                                                            nullptr,
                                                            detail::role::server,
                                                            options.connection,
                                                            subprotocol,
                                                            options.stop.stop_possible() ? options.stop : req.stop)};
}

task<std::expected<connection, std::error_code>>
connect(const http::client& client, std::string_view url, const connect_options& options)
{
    // NOTE: not a coroutine, so url is parsed (and options copied) before this returns, rather than when the task
    //       starts (when they might be gone).
    auto where = endpoint_of(url);
    if (!where.has_value())
        return ready_task<std::expected<connection, std::error_code>>(std::unexpected(where.error()));

    if (!valid_sizes(options.connection) || options.read_buffer_size < http::detail::min_buffer_size
        || options.write_buffer_size < http::detail::min_buffer_size)
    {
        return ready_task<std::expected<connection, std::error_code>>(
            std::unexpected(std::make_error_code(std::errc::invalid_argument)));
    }

    return open(http::detail::pool_of(client), std::move(*where), options);
}

connection::connection(std::unique_ptr<detail::connection_state> state) noexcept
    : state{std::move(state)}
{}

connection::connection(connection&& other) noexcept = default;

connection& connection::operator=(connection&& other) noexcept
{
    if (this != &other)
    {
        connection old{std::move(*this)};
        state = std::move(other.state);
    }
    return *this;
}

connection::~connection()
{
    if (!state) return;

    // without this side's close, the peer sees the connection end without one.
    bool closed = false;
    {
        std::scoped_lock lock{state->mu};
        closed = state->sess.state() == detail::close_state::close_sent
              || state->sess.state() == detail::close_state::closed;
    }
    if (!closed) state->tunnel.abort();
}

task<std::expected<message_type, std::error_code>> connection::read_message(std::string& message)
{
    return read_whole(state.get(), message);
}

task<std::expected<message_type, std::error_code>> connection::read_message(std::vector<std::byte>& message)
{
    return read_whole(state.get(), message);
}

task<std::expected<message_type, std::error_code>> connection::next_message()
{
    using goal   = connection_state::goal;
    using pumped = connection_state::pumped;
    using result = std::expected<message_type, std::error_code>;

    auto* s = state.get();
    if (auto err = s->begin_read()) return ready_task<result>(std::unexpected(err));

    s->skipping = s->in_message;

    connection_state::target t;
    const auto               step = s->pump(goal::begin, t);
    if (step == pumped::done)
    {
        s->end_read();
        return ready_task<result>(s->type);
    }

    return s->drive(goal::begin,
                    t,
                    step,
                    [s](std::error_code err, const connection_state::target& /*t*/) -> result
                    {
                        if (err) return std::unexpected(err);
                        return s->type;
                    });
}

io::reader& connection::message() noexcept { return state->stream; }

task<std::error_code> connection::write_message(message_type type, std::span<const std::byte> data)
{
    using use = connection_state::frame_use;
    return state->write_frame(use::message, static_cast<detail::opcode>(type), data);
}

task<std::expected<message_writer, std::error_code>> connection::begin_message(message_type type)
{
    using result = std::expected<message_writer, std::error_code>;

    // begin takes the message turn, if it's free, saying whether it did, or why it failed.
    const auto begin = [](connection_state* state, message_type type) -> std::optional<result>
    {
        std::scoped_lock lock{state->mu};
        if (state->failed) return std::unexpected(state->failed);
        if (state->sess.state() == detail::close_state::close_sent
            || state->sess.state() == detail::close_state::closed)
            return std::unexpected(std::make_error_code(std::errc::broken_pipe));
        if (state->message_open) return std::nullopt;

        state->message_open  = true;
        state->message_first = true;
        state->message_op    = static_cast<detail::opcode>(type);
        return message_writer{state, ++state->message_count};
    };

    if (auto begun = begin(state.get(), type)) return ready_task<result>(std::move(*begun));

    return [](connection_state* state, message_type type, auto begin) -> task<result>
    {
        while (true)
        {
            const auto seen = state->turn.value();
            if (auto begun = begin(state, type)) co_return std::move(*begun);
            (void)co_await state->turn.wait(seen);
        }
    }(state.get(), type, begin);
}

task<std::error_code> connection::ping(std::span<const std::byte> data)
{
    if (data.size() > detail::max_control_payload) return ready_task<std::error_code>(invalid());

    using use = connection_state::frame_use;
    return state->write_frame(use::control, detail::opcode::ping, data);
}

task<std::error_code> connection::close(close_code code, std::string_view reason)
{
    std::array<std::byte, detail::max_control_payload> payload{};
    auto                                               size = detail::encode_close(code, reason, payload);
    if (!size) return ready_task<std::error_code>(size.error());

    return state->close(payload, *size);
}

std::string_view connection::subprotocol() const noexcept { return state->subprotocol; }

std::string_view connection::close_reason() const noexcept { return {state->reason.data(), state->reason_size}; }

task<io::result> message_writer::write(std::span<const std::byte> data)
{
    if (data.empty()) return ready_task<io::result>();

    using use = connection_state::frame_use;
    return as_result(state->write_frame(use::part, detail::opcode::continuation, data, message), data.size());
}

task<std::error_code> message_writer::end_message()
{
    using use = connection_state::frame_use;
    return state->write_frame(use::message_end, detail::opcode::continuation, {}, message);
}

io::handle message_writer::native_handle() const noexcept { return state->tunnel.writer().native_handle(); }

}
