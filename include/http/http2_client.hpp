#pragma once

#include <cstddef>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include <spdlog/logger.h>

#include "coro/task.hpp"
#include "http/connection.hpp"
#include "http/headers.hpp"
#include "http/hpack.hpp"
#include "http/http2.hpp"
#include "http/http2_config.hpp"
#include "http/http2_connection.hpp"
#include "http/origin.hpp"
#include "http/priority.hpp"
#include "http/request.hpp"
#include "http/response.hpp"
#include "io/scheduler.hpp"

// HTTP/2 in http::client (see docs/plans/http2.md): the client's pool's, not for applications.
namespace net::http::detail
{

// http2_client_stream is a request sent on a client's HTTP/2 connection, and its response as it arrives.
class http2_client_stream final : public http2_stream_base
{
public:
    using http2_stream_base::http2_stream_base;

    // Guarded by the connection's lock.
    // NOTE: the status and the flags together, so they take one word.
    status status_code   = status::NONE;
    bool   head_received = false;
    bool   processed     = false; // not refused (it may not be retried)
    bool   wants_interim = false; // the request has on_informational (see interims)

    // A request expecting 100 (Continue) waits for it before sending its body (see http2_client_connection::send()):
    // continued is set once a 100 has come (whether or not the request wants interim responses), and continue_waited
    // once the wait's timed out (by the connection's timer, at continue_deadline, while awaiting_continue: without
    // failing the stream).
    bool                  continued         = false;
    bool                  continue_waited   = false;
    bool                  awaiting_continue = false;
    io::clock::time_point continue_deadline{};

    net::http::headers headers;

    // interim_head is an informational (1xx) response's head.
    struct interim_head
    {
        status             status_code = status::NONE;
        net::http::headers headers;
    };

    // The informational responses received, kept for the request's on_informational (if it has one: wants_interim)
    // until send()'s task passes them on; and what they've all come to (by hpack::field_size), whether or not they're
    // kept, which is limited (see on_headers()).
    // NOTE: wants_interim is with the flags above.
    std::vector<interim_head> interims;
    std::size_t               interim_size = 0;

    // for sending the request's head, reused.
    std::vector<std::string>  names;
    std::vector<hpack::field> fields;
    std::string               authority;
    std::string               path;
};

// origin_set is an HTTP/2 connection's Origin Set (RFC 8336, section 2.3; docs/plans/http2.md, stage 6's eleventh
// part), once the server's first ORIGIN frame has initialized it, and what's known of its certificate for each of its
// hosts. It's guarded by the connection's lock.
struct origin_set
{
    // How many origins it keeps: those past it are ignored (RFC 8336 sets no limit).
    static constexpr std::size_t max_origins = 256;

    // The origins, normalized (see parse_origin()), each once, in the order they came: the connection's own first.
    std::vector<https_origin> origins;

    // The hosts whose certificate has been checked (see tls::detail::verify_for()), and whether it was valid for each:
    // so each is checked once per connection. And how many checks have been made (for tests: two at once may both
    // check a host).
    std::vector<std::pair<std::string, bool>> checked;
    std::size_t                               checks = 0;

    // find is the origin with serialized as its serialization (normalized), if it has it.
    [[nodiscard]] const https_origin* find(std::string_view serialized) const noexcept;

    // add adds o, unless it has it, or is full.
    void add(https_origin o);

    // checked_for is whether the certificate is valid for host, if that's been checked.
    [[nodiscard]] std::optional<bool> checked_for(std::string_view host) const noexcept;
};

// owned_connection holds what an http2_client_connection owns, before its base (which refers to the connection) is
// made.
struct owned_connection
{
    std::unique_ptr<connection>     owned;
    std::shared_ptr<spdlog::logger> log;
};

// http2_client_connection is an HTTP/2 connection that a client's requests share, each on a stream of its own.
class http2_client_connection final
    : private owned_connection // NOTE: first, so it's made before the base
    , public http2_connection_base
    , public std::enable_shared_from_this<http2_client_connection>
{
public:
    // closed is called (with the connection) once it has ended; freed whenever a stream is done (so another may be
    // sent); origins_known (with the connection) once its Origin Set's been initialized (see origins()), by the
    // server's first ORIGIN frame, not holding the connection's lock: for the pool's coalescing (docs/plans/http2.md,
    // stage 6's twelfth part), which finds the connections with origin sets through it.
    struct callbacks
    {
        std::function<void(http2_client_connection&)> closed;
        std::function<void()>                         freed;
        std::function<void(http2_client_connection&)> origins_known;
    };

    // With options.notsent_lowat, it sets TCP_NOTSENT_LOWAT on the connection's socket, and if that works, its writer
    // waits for the socket to take more before each round (see http2_connection_base::write_loop()).
    //
    // origin is the connection's own (see origin_text(): https, the host it connected to, its port), the first of its
    // Origin Set, once ORIGIN initializes it; none, or one that doesn't parse (see parse_origin()), leaves it out. It's
    // only kept over TLS, with the server's certificate verified (see origins()).
    http2_client_connection(io::scheduler*              scheduler,
                            std::unique_ptr<connection> conn,
                            const http2_client_options& options,
                            callbacks                   on,
                            std::string                 origin = {});

    // send_state is how far send() got with a request that failed, for deciding whether it may be sent again (on
    // another connection).
    struct send_state
    {
        // Refused before its stream was opened (the connection was ending, or had the server's GOAWAY): nothing of it
        // was sent, nor its body read.
        bool unsent = false;

        // The connection ended, without the server's GOAWAY, before the response's head (or any informational
        // response): the server may or may not have processed it.
        bool dropped = false;

        // It expected 100 (Continue), and its response (returned) is a 417 (Expectation Failed) that came before its
        // body was sent: so it may be sent again, without the expectation (see client::send()).
        bool expectation_failed = false;
    };

    // start runs the connection, as one of the scheduler's tasks, returning false if the scheduler's stopping.
    bool start();

    // reserve reserves a stream for a request, which its send() takes, if the connection's open and has one free
    // (counting those reserved already), returning whether it did. Without one, send() may fail with refused_stream.
    [[nodiscard]] bool reserve();

    // idle is whether it has no streams open, nor any reserved.
    [[nodiscard]] bool idle();

    // idle_since is when it became idle, if it is.
    [[nodiscard]] std::optional<io::clock::time_point> idle_since();

    // accepting is whether it may still open streams: it's not ending, nor been sent GOAWAY by the server, nor
    // draining. (It may still have none free.)
    [[nodiscard]] bool accepting();

    // send sends a request, returning the response once its head has arrived. It fails with refused_stream (in
    // http2::category()) if the server didn't process the request, so it may be sent again; state (if given) says how
    // far it got. A request with a Content-Length sends exactly that much of its body, failing with
    // std::errc::invalid_argument if the body's shorter. It takes the stream reserve() reserved, whatever happens.
    //
    // Its body takes its turns on the connection (with other requests' bodies sent at once) by its Priority field (RFC
    // 9218), read as the head's sent, as the server reads it (what it leaves out, or all of it if it doesn't parse, is
    // RFC 9218's default: u=3, not incremental); without the field, u=3 incremental.
    //
    // request.stop cancels it (and then the response's body): the stream is reset (CANCEL), failing its waits with
    // std::errc::operation_canceled, and the connection goes on. Stopped before the stream's opened, nothing's sent.
    //
    // A request expecting 100 (Continue) (see detail::expects_continue(), with the options' expect_continue_timeout)
    // sends its head without ending the stream, then waits before sending its body (docs/plans/http2.md, stage 6's
    // sixth part): a 100, or none within the timeout, sends it; another interim response is passed on, and the wait
    // goes on. A final response first, above 299, has the body never sent (the stream's reset once the response is
    // done with, unless the server's reset it), and a 417 says so in state (expectation_failed); a 2xx goes on waiting
    // for the timeout, then sends it (as Go's client does, for streams that are full-duplex).
    coro::task<std::expected<client_response, std::error_code>> send(client_request& request,
                                                                     send_state*     state = nullptr);

    // close ends the connection (GOAWAY), failing the streams still open.
    void close();

    // drain sends GOAWAY, and ends the connection once its streams are done (at once, if it has none): no more are
    // opened.
    void drain();

    // done is a request being done with its stream: its response's body, or send() if the request failed (resetting
    // it if it's not finished, and telling the pool it's free).
    void done(http2::stream_id id);

    // reprioritize changes a request's priority once its response's head has come (client_response::set_priority(),
    // through its body; docs/plans/http2.md, stage 6's ninth part): it sends the server a PRIORITY_UPDATE for the
    // stream (only if it changes what the stream last said: see http2::session::submit_priority_update()), and moves
    // the request's body in the connection's scheduler, should it still be waiting there. It returns false, doing
    // nothing, once the response has ended (or failed), or the stream's done with. It takes the lock.
    bool reprioritize(http2::stream_id id, priority p);

    // The connection's Origin Set (RFC 8336; docs/plans/http2.md, stage 6's eleventh part): what the server's ORIGIN
    // frames say it's authoritative for, for the pool's coalescing (its twelfth part). Over TLS only, with the server's
    // certificate verified (not with insecure_skip_verification, as nothing then says the server is anyone's), it's
    // initialized by the server's first ORIGIN frame, with the connection's own origin and the frame's entries; later
    // frames add to it. Each entry is parsed as an https origin, normalized (see parse_origin()), and ignored if it
    // doesn't parse (RFC 8336: "If parsing fails, the field MUST be ignored"; an origin of another scheme too, as http
    // requests never go over TLS). It keeps at most origin_set::max_origins, ignoring the rest.

    // origins is the Origin Set's origins, in the order they came (the connection's own first), if it's initialized.
    [[nodiscard]] std::optional<std::vector<std::string>> origins();

    // authoritative_for is whether the connection may take requests for origin (an https origin, as parse_origin()
    // reads it) by its Origin Set: it's initialized and has the origin, and the server's certificate is valid for its
    // host, as a new connection's handshake would check it (tls::detail::verify_for(), Decided question 37): checked
    // the first time a host's asked for, without the lock (it costs what a handshake's verification does), then
    // remembered. False without an initialized set: coalescing is by ORIGIN only (Decided question 35).
    [[nodiscard]] bool authoritative_for(std::string_view origin);

    // remove_origin removes origin from the Origin Set (RFC 8336's MUST, when a request for it gets 421 (Misdirected
    // Request): see Decided question 40), returning whether it was there. The connection's own may be removed too.
    bool remove_origin(std::string_view origin);

    // certificate_checks is how many times the server's certificate has been checked for a host (see
    // authoritative_for()), for tests.
    [[nodiscard]] std::size_t certificate_checks();

    // cancel fails a request's stream with std::errc::operation_canceled (unless it's failed already), and resets it
    // (CANCEL), if it's still there: a request's stop callback (see canceler), so it takes the lock, and must not be
    // called holding it. The stream stays the request's until done().
    void cancel(http2::stream_id id);

    // canceler is a request's std::stop_callback: it cancels its stream. Its connection must outlive it.
    struct canceler
    {
        http2_client_connection* conn;
        http2::stream_id         id;

        // NOTE: cancel() calls on.freed only if it's set, and only throws if it can't allocate, when terminating is
        //       fine.
        // NOLINTNEXTLINE(bugprone-exception-escape)
        void operator()() const noexcept { conn->cancel(id); }
    };

    // NOTE: the callback runs on whichever thread requests stop (inline, if it was already), and destroying it waits
    // for
    //       it if it's running elsewhere: so it's never registered nor destroyed holding the lock, which it takes.
    using stop_registration = std::stop_callback<canceler>;

protected:
    // the session's events, and the base's hooks: protected, as they're declared.
    void on_headers(http2::stream_id              id,
                    http2::block_kind             kind,
                    std::span<const hpack::field> fields,
                    bool                          end_stream) override;
    void on_header_list_too_large(http2::stream_id stream, bool end_stream) override;
    void on_goaway(http2::stream_id last, std::error_code err, std::string_view debug) override;
    void on_ping_ack(std::span<const std::byte, 8> opaque) override;
    void on_priority_update(http2::stream_id stream, priority p) override;
    void on_origin(std::span<const std::string_view> entries) override;

    void                                 after_receive() override;
    std::optional<io::clock::time_point> check_connection(io::clock::time_point now) override;
    void                                 maybe_end() override;

private:
    // open opens a stream for request, sending its head, holding the lock: unless its stop was requested (failing
    // with std::errc::operation_canceled). Either way, it takes the stream reserved for it, if there was one.
    // ends_after is how much DATA ends the stream, if that's known (no trailers follow): 0 ends it with the head, more
    // with the body's last DATA frame (see http2_stream_base::data_left); none leaves it to end_request().
    std::expected<std::shared_ptr<http2_client_stream>, std::error_code> open(const client_request&      request,
                                                                              std::optional<std::size_t> ends_after);

    // body_result is what sending a request's body came to: why it stopped short, if it did; and whether that's the
    // body's fault (it failed, or was shorter than its Content-Length), rather than the stream's.
    struct body_result
    {
        std::error_code err;
        bool            body_failed = false;
    };

    // send_body sends the request's body on the stream (length bytes of it, if that's set), then ends it (see
    // end_request()).
    coro::task<body_result>
    send_body(client_request& request, http2_client_stream& stream, std::optional<std::size_t> length);

    // end_request ends the request on the stream, once its body's been sent: with make_trailers' fields, if it's set
    // and leaves any, otherwise an empty DATA frame (unless the stream's ended, e.g. with the body's last DATA, or
    // failed). It fails with std::errc::invalid_argument, sending nothing, if a trailer can't be sent (see
    // append_fields()); whatever make_trailers throws comes out of it.
    std::error_code end_request(const client_request& request, http2_client_stream& stream);

    // abandon resets a stream whose request can't be finished, so the server doesn't take what it has as all of it.
    void abandon(http2_client_stream& stream, std::error_code err);

    // await_continue waits for what decides whether the body of a request expecting 100 (Continue) is sent, its head
    // having been sent (see send()), passing on the interim responses that come meanwhile: true for a 100, or none in
    // time (also after a 2xx); false for a final response above 299 (a 417 noted in state, if it's given), or the
    // stream failing.
    coro::task<bool> await_continue(http2_client_stream& stream, const client_request& request, send_state* state);

    // expect_continue starts a stream's wait for 100 (Continue), to the timeout, telling the timer; stop_awaiting ends
    // it, if it's on. Both holding the lock.
    void expect_continue(http2_client_stream& s);
    void stop_awaiting(http2_client_stream& s) noexcept;

    // check_continues ends the waits for 100 (Continue) that have timed out (holding the lock), returning when the
    // next does.
    std::optional<io::clock::time_point> check_continues(io::clock::time_point now);

    // on_interim takes an informational response's head (holding the lock: on_headers()'s).
    void on_interim(http2_client_stream& s, std::span<const hpack::field> fields);

    // idle_now is whether it has no streams, nor any reserved (holding the lock).
    [[nodiscard]] bool idle_now() const noexcept { return streams.empty() && reserved == 0; }

    // wait_for_head waits for the response's head, returning the response (with its body, canceled by request.stop),
    // or why it didn't come (and in state, whether the connection dropped first). It passes the informational
    // responses that come first to request.on_informational, in order, without the lock.
    coro::task<std::expected<client_response, std::error_code>>
    wait_for_head(std::shared_ptr<http2_client_stream> stream,
                  std::optional<std::error_code>       body_error,
                  send_state*                          state,
                  const client_request&                request);

    static coro::task<> run_owned(std::shared_ptr<http2_client_connection> self);

    http2_client_options options;
    callbacks            on;

    // Guarded by mu.
    std::unordered_map<http2::stream_id, std::shared_ptr<http2_client_stream>> streams;
    std::size_t                                                                reserved    = 0;
    bool                                                                       peer_goaway = false;
    bool                                                                       draining    = false; // drain()ed
    // on.origins_known is to be called (after the receive whose ORIGIN initialized the Origin Set, without the lock).
    bool                                 tell_origins = false;
    std::optional<io::clock::time_point> idle_from = io::clock::now(); // when it was last idle (none: not yet looked)
    std::size_t open_seen   = 0; // the session's open streams, when last looked at: fewer now means one's been freed
    bool        goaway_told = false; // the pool's been told of the server's GOAWAY (as a stream freed)

    // Set when it's made (so not guarded): whether it keeps an Origin Set at all: over TLS (RFC 8336 has ORIGIN ignored
    // over cleartext), verifying the server's certificate (without, nothing says the server is anyone's, its own
    // origin's included). NOTE: here, in what was padding, and before own_origin, which is made from it.
    bool keeps_origins = false;

    std::size_t awaiting_continue = 0; // streams waiting for 100 (Continue): none, and the timer doesn't look for them

    // The Origin Set, once ORIGIN has initialized it: on the heap, as few connections get one.
    std::unique_ptr<origin_set> known_origins;

    // Set when it's made: the connection's own origin, the Origin Set's first (only if it keeps one).
    std::string own_origin;
};

}
