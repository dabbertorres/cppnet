#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>

#include "coro/task.hpp"
#include "http/chunked_writer.hpp"
#include "http/headers.hpp"
#include "http/http.hpp"
#include "http/priority.hpp"
#include "http/request.hpp"
#include "io/buffered_reader.hpp"
#include "io/io.hpp"
#include "io/limit_writer.hpp"
#include "io/reader.hpp"
#include "io/writer.hpp"
#include "url.hpp"
#include "util/function_ref.hpp"

namespace net::http
{

enum class status : std::uint16_t
{
    NONE = 0,

    // 1×× Informational
    CONTINUE            = 100,
    SWITCHING_PROTOCOLS = 101,
    PROCESSING          = 102,
    EARLY_HINTS         = 103,

    // 2×× Success
    OK                            = 200,
    CREATED                       = 201,
    ACCEPTED                      = 202,
    NON_AUTHORITATIVE_INFORMATION = 203,
    NO_CONTENT                    = 204,
    RESET_CONTENT                 = 205,
    PARTIAL_CONTENT               = 206,
    MULTI_STATUS                  = 207,
    ALREADY_REPORTED              = 208,
    IM_USED                       = 226,

    // 3×× Redirection
    MULTIPLE_CHOICES   = 300,
    MOVED_PERMANENTLY  = 301,
    FOUND              = 302,
    SEE_OTHER          = 303,
    NOT_MODIFIED       = 304,
    USE_PROXY          = 305,
    TEMPORARY_REDIRECT = 307,
    PERMANENT_REDIRECT = 308,

    // 4×× Client Error
    BAD_REQUEST                        = 400,
    UNAUTHORIZED                       = 401,
    PAYMENT_REQUIRED                   = 402,
    FORBIDDEN                          = 403,
    NOT_FOUND                          = 404,
    METHOD_NOT_ALLOWED                 = 405,
    NOT_ACCEPTABLE                     = 406,
    PROXY_AUTHENTICATION_REQUIRED      = 407,
    REQUEST_TIMEOUT                    = 408,
    CONFLICT                           = 409,
    GONE                               = 410,
    LENGTH_REQUIRED                    = 411,
    PRECONDITION_FAILED                = 412,
    PAYLOAD_TOO_LARGE                  = 413,
    REQUEST_URI_TOO_LONG               = 414,
    UNSUPPORTED_MEDIA_TYPE             = 415,
    REQUESTED_RANGE_NOT_SATISFIABLE    = 416,
    EXPECTATION_FAILED                 = 417,
    IM_A_TEAPOT                        = 418,
    MISDIRECTED_REQUEST                = 421,
    UNPROCESSABLE_ENTITY               = 422,
    LOCKED                             = 423,
    FAILED_DEPENDENCY                  = 424,
    UPGRADE_REQUIRED                   = 426,
    PRECONDITION_REQUIRED              = 428,
    TOO_MANY_REQUESTS                  = 429,
    REQUEST_HEADER_FIELDS_TOO_LARGE    = 431,
    CONNECTION_CLOSED_WITHOUT_RESPONSE = 444,
    UNAVAILABLE_FOR_LEGAL_REASONS      = 451,
    CLIENT_CLOSED_REQUEST              = 499,

    // 5×× Server Error
    INTERNAL_SERVER_ERROR           = 500,
    NOT_IMPLEMENTED                 = 501,
    BAD_GATEWAY                     = 502,
    SERVICE_UNAVAILABLE             = 503,
    GATEWAY_TIMEOUT                 = 504,
    HTTP_VERSION_NOT_SUPPORTED      = 505,
    VARIANT_ALSO_NEGOTIATES         = 506,
    INSUFFICIENT_STORAGE            = 507,
    LOOP_DETECTED                   = 508,
    NOT_EXTENDED                    = 510,
    NETWORK_AUTHENTICATION_REQUIRED = 511,
    NETWORK_CONNECT_TIMEOUT_ERROR   = 599,
};

status parse_status(std::string_view str) noexcept;

// format_date formats t as an HTTP date (RFC 9110, section 5.6.7), to the second: e.g. "Sun, 06 Nov 1994 08:49:37 GMT".
std::string format_date(std::chrono::system_clock::time_point t);

// reason_phrase is the status's reason phrase (RFC 9110, section 15), or empty for a status it doesn't know.
constexpr std::string_view reason_phrase(status s) noexcept
{
    using enum status;
    switch (s)
    {
    case CONTINUE: return "Continue";
    case SWITCHING_PROTOCOLS: return "Switching Protocols";
    case PROCESSING: return "Processing";
    case EARLY_HINTS: return "Early Hints";

    case OK: return "OK";
    case CREATED: return "Created";
    case ACCEPTED: return "Accepted";
    case NON_AUTHORITATIVE_INFORMATION: return "Non-Authoritative Information";
    case NO_CONTENT: return "No Content";
    case RESET_CONTENT: return "Reset Content";
    case PARTIAL_CONTENT: return "Partial Content";
    case MULTI_STATUS: return "Multi-Status";
    case ALREADY_REPORTED: return "Already Reported";
    case IM_USED: return "IM Used";

    case MULTIPLE_CHOICES: return "Multiple Choices";
    case MOVED_PERMANENTLY: return "Moved Permanently";
    case FOUND: return "Found";
    case SEE_OTHER: return "See Other";
    case NOT_MODIFIED: return "Not Modified";
    case USE_PROXY: return "Use Proxy";
    case TEMPORARY_REDIRECT: return "Temporary Redirect";
    case PERMANENT_REDIRECT: return "Permanent Redirect";

    case BAD_REQUEST: return "Bad Request";
    case UNAUTHORIZED: return "Unauthorized";
    case PAYMENT_REQUIRED: return "Payment Required";
    case FORBIDDEN: return "Forbidden";
    case NOT_FOUND: return "Not Found";
    case METHOD_NOT_ALLOWED: return "Method Not Allowed";
    case NOT_ACCEPTABLE: return "Not Acceptable";
    case PROXY_AUTHENTICATION_REQUIRED: return "Proxy Authentication Required";
    case REQUEST_TIMEOUT: return "Request Timeout";
    case CONFLICT: return "Conflict";
    case GONE: return "Gone";
    case LENGTH_REQUIRED: return "Length Required";
    case PRECONDITION_FAILED: return "Precondition Failed";
    case PAYLOAD_TOO_LARGE: return "Payload Too Large";
    case REQUEST_URI_TOO_LONG: return "Request-Uri Too Long";
    case UNSUPPORTED_MEDIA_TYPE: return "Unsupported Media Type";
    case REQUESTED_RANGE_NOT_SATISFIABLE: return "Requested Range Not Satisfiable";
    case EXPECTATION_FAILED: return "Expectation Failed";
    case IM_A_TEAPOT: return "I'm A Teapot";
    case MISDIRECTED_REQUEST: return "Misdirected Request";
    case UNPROCESSABLE_ENTITY: return "Unprocessable Entity";
    case LOCKED: return "Locked";
    case FAILED_DEPENDENCY: return "Failed Dependency";
    case UPGRADE_REQUIRED: return "Upgrade Required";
    case PRECONDITION_REQUIRED: return "Precondition Required";
    case TOO_MANY_REQUESTS: return "Too Many Requests";
    case REQUEST_HEADER_FIELDS_TOO_LARGE: return "Request Header Fields Too Large";
    case CONNECTION_CLOSED_WITHOUT_RESPONSE: return "Connection Closed Without Response";
    case UNAVAILABLE_FOR_LEGAL_REASONS: return "Unavailable For Legal Reasons";
    case CLIENT_CLOSED_REQUEST: return "Client Closed Request";

    case INTERNAL_SERVER_ERROR: return "Internal Server Error";
    case NOT_IMPLEMENTED: return "Not Implemented";
    case BAD_GATEWAY: return "Bad Gateway";
    case SERVICE_UNAVAILABLE: return "Service Unavailable";
    case GATEWAY_TIMEOUT: return "Gateway Timeout";
    case HTTP_VERSION_NOT_SUPPORTED: return "HTTP Version Not Supported";
    case VARIANT_ALSO_NEGOTIATES: return "Variant Also Negotiates";
    case INSUFFICIENT_STORAGE: return "Insufficient Storage";
    case LOOP_DETECTED: return "Loop Detected";
    case NOT_EXTENDED: return "Not Extended";
    case NETWORK_AUTHENTICATION_REQUIRED: return "Network Authentication Required";
    case NETWORK_CONNECT_TIMEOUT_ERROR: return "Network Connect Timeout Error";

    default: return "";
    }
}

constexpr bool status_is_information(status s) noexcept { return status::CONTINUE <= s && s < status::OK; }
constexpr bool status_is_success(status s) noexcept { return status::OK <= s && s < status::MULTIPLE_CHOICES; }
constexpr bool status_is_redirection(status s) noexcept
{
    return status::MULTIPLE_CHOICES <= s && s < status::BAD_REQUEST;
}
constexpr bool status_is_client_error(status s) noexcept
{
    return status::BAD_REQUEST <= s && s < status::INTERNAL_SERVER_ERROR;
}
constexpr bool status_is_server_error(status s) noexcept { return status::INTERNAL_SERVER_ERROR <= s; }

namespace detail
{
    // body_source is what the client's response bodies implement for what client_response asks of its body, which it
    // finds with a dynamic_cast (so client_response holds nothing more for them): its trailers() and set_priority().
    class body_source
    {
    public:
        // trailers are the body's trailer fields: empty until a read has reported its end, and for a body without
        // any. Valid as long as the body is.
        [[nodiscard]] virtual const trailer_fields& trailers() const noexcept = 0;

        // set_priority asks the server to send the rest of the body with priority p, returning whether it could (see
        // client_response::set_priority()).
        virtual bool set_priority(const priority& p) = 0;

    protected:
        body_source() noexcept                              = default;
        body_source(const body_source&) noexcept            = default;
        body_source& operator=(const body_source&) noexcept = default;
        body_source(body_source&&) noexcept                 = default;
        body_source& operator=(body_source&&) noexcept      = default;
        ~body_source()                                      = default;
    };
}

// client_response represents an incoming HTTP response from a server to a client.
struct client_response
{
    protocol_version   version{};
    status             status_code = status::NONE;
    net::http::headers headers{};

    // The body, which ends (io::errc::end_of_stream) only once all of it has been read: one cut short fails (with
    // std::errc::connection_aborted, if the connection ends first).
    std::unique_ptr<io::reader> body = nullptr;

    // The URL of the request the response answers: the request's, or after redirects, the last one's (e.g. to resolve
    // the body's relative links against).
    net::url url{};

    client_response() noexcept = default;

    client_response(const client_response&)            = delete;
    client_response& operator=(const client_response&) = delete;

    client_response(client_response&& other) noexcept
        : version{std::exchange(other.version, {})}
        , status_code{std::exchange(other.status_code, status::NONE)}
        , headers{std::exchange(other.headers, {})}
        , body{std::exchange(other.body, nullptr)}
        , url{std::exchange(other.url, {})}
    {}

    client_response& operator=(client_response&& other) noexcept
    {
        if (this != std::addressof(other))
        {
            version     = std::exchange(other.version, {});
            status_code = std::exchange(other.status_code, status::NONE);
            headers     = std::exchange(other.headers, {});
            body        = std::exchange(other.body, nullptr);
            url         = std::exchange(other.url, {});
        }

        return *this;
    }

    ~client_response() noexcept = default;

    // trailers are the response's trailer fields, once its body has been read to its end (a read has reported
    // io::errc::end_of_stream): over HTTP/1.1, a chunked body's trailer section; over HTTP/2, a HEADERS frame after the
    // body. Empty until then, for a response without any, and for a body that isn't the client's (e.g. one the caller
    // put in its place). They're kept apart from headers (RFC 9110, section 6.5), and without the fields that can't be
    // trailers (see trailer_allowed()), which are dropped. They're the body's: valid as long as it is.
    [[nodiscard]] const net::http::trailer_fields& trailers() const noexcept;

    // set_priority asks the server to send the rest of the response with priority p (RFC 9218; e.g. {.urgency = 7}
    // for a download that's become background): over HTTP/2, a PRIORITY_UPDATE frame for its stream (an urgency past
    // 7 sent as 7), which the server may ignore. It returns false, sending nothing, over HTTP/1.1, for a body that
    // isn't the client's (e.g. one the caller put in its place), or once the response has ended (the server's sent all
    // of it, though it may not all have been read yet) or failed. Repeating what was last asked sends nothing more, and
    // updates made faster than the connection's writer takes them are sent as the last of them. Any thread may call
    // it, while the body's being read too.
    //
    // (The request's own body, if it were still being sent, would take p's turns among the connection's other
    // uploads; but send() returns once it's all gone, so by then it has.)
    bool set_priority(const priority& p);
};

// server_response represents an outgoing HTTP response from a server to a client.
struct server_response
{
    protocol_version   version{};
    status             status_code = status::NONE;
    net::http::headers headers{};
    io::writer*        body = nullptr;

    // The response's trailer fields, sent after the body (see response_writer::trailers()).
    net::http::trailer_fields trailers{};
};

using response_encoder_result = std::expected<io::writer*, std::error_code>;
using response_decoder_result = std::expected<client_response, std::error_code>;

using response_encoder = coro::task<response_encoder_result> (*)(io::writer*, const server_response&) noexcept;
using response_decoder =
    coro::task<response_decoder_result> (*)(std::unique_ptr<io::buffered_reader>,
                                            request_method,
                                            std::size_t,
                                            util::function_ref<void(status, const headers&)>) noexcept;

struct response_writer;

namespace detail
{
    // http2_stream is an HTTP/2 stream the server serves (see http/http2_server.hpp).
    class http2_stream;

    // tunnel is a connection's bytes once it has switched protocols (see http/tunnel.hpp).
    class tunnel;

    // switch_hook is how a response_writer reaches the connection it responds on, to switch it: made once per
    // connection (a function and its context, as io::start_callback is), so a response_writer holds only a pointer to
    // it. call readies the connection, on the handler's task, and returns its tunnel: from then, nothing but the
    // tunnel's user reads the connection (e.g. the server's watch for the client leaving, which it ends if one's in
    // progress, keeping what it read), reads don't flush what's written, and they have no timeout. It returns null,
    // having done nothing, if what follows the request's head isn't the new protocol's yet: its body, which hasn't been
    // read to its end.
    struct switch_hook
    {
        coro::task<tunnel*> (*call)(void* context) = nullptr;
        void* context                              = nullptr;
    };

    // switch_protocols switches the connection rw's request came on to another protocol (see http/tunnel.hpp).
    coro::task<std::expected<tunnel*, std::error_code>> switch_protocols(response_writer& rw, fields_view fields);

    // http2_stream_ops are what a response_writer does with an HTTP/2 stream, which its server provides: so that
    // response_writer doesn't depend on HTTP/2's server (as it doesn't on HTTP/1.1's encoder).
    struct http2_stream_ops
    {
        // send_head sends a response's head, returning the writer for its body. body_length is the body's length,
        // if the head gives it (its Content-Length, or 0 for a status without content): the stream ends with the head
        // if it's 0, and with the body's last DATA frame otherwise, unless trailers (or a Trailer field) are set by
        // then, which end it instead (docs/plans/http2.md, Decided question 26). None for a body of unknown length,
        // which finish() ends, and for an interim response.
        coro::task<response_encoder_result> (*send_head)(http2_stream&              stream,
                                                         const server_response&     resp,
                                                         std::optional<std::size_t> body_length);

        // finish ends the stream's response, if it hasn't been.
        coro::task<io::result> (*finish)(http2_stream& stream);
    };
}

// response_writer presents an interface for building a server_response to request handlers.
struct response_writer
{
public:
    // An HTTP/1.x response, whose head encoder writes to writer. Once close_after (if given) is stopped, a final
    // response not yet sent says the connection closes after it (Connection: close): the server's close() does that.
    // switching (if given) is the connection's, for detail::switch_protocols().
    response_writer(io::writer*                writer,
                    server_response*           base,
                    response_encoder           encoder,
                    const std::stop_token*     close_after = nullptr,
                    const detail::switch_hook* switching   = nullptr);

    // An HTTP/2 response, on stream.
    response_writer(detail::http2_stream* stream, const detail::http2_stream_ops* ops, server_response* base) noexcept;

    net::http::headers& headers() noexcept;

    // trailers are the response's trailer fields (e.g. Server-Timing, or gRPC's grpc-status), sent after the body when
    // it ends (finish(), which the server calls once the handler returns, so middleware may add to them after the
    // handler it wraps): over HTTP/2, as a final HEADERS frame, whatever the body's framing; over HTTP/1.1, in the last
    // chunk, so only for a body sent without a Content-Length (send(status)) to an HTTP/1.1 client, and dropped
    // otherwise (with a Content-Length, to an HTTP/1.0 client, and for HEAD). They're sent whatever the request's TE.
    // Fields that can't be trailers are dropped (see trailer_allowed()); an invalid name or value (e.g. a line break)
    // fails finish() with std::errc::invalid_argument, and the response is cut short (the connection closed, or the
    // stream reset), so the client doesn't take it as complete. Over HTTP/2, a response with a Content-Length (or
    // without a body) ends with its last DATA frame (or its head), unless trailers (or a Trailer field, which declares
    // them, as RFC 9110 says a sender should) are set by the time its head is sent: then they follow the body, and
    // trailers set later, undeclared, are dropped. So middleware that adds trailers after the handler it wraps declares
    // them (Trailer) before calling it. A response sent with send(status) is ended by finish(), whenever they're set.
    net::http::trailer_fields& trailers() noexcept;

    // send sends the response's status and headers, with a Content-Length of content_length (except for statuses
    // that have no content: 1xx, 204 and 304), and returns the writer for its body. Writing more than content_length
    // fails with std::errc::value_too_large; writing less leaves the server to close the connection. It throws
    // std::system_error if the response can't be sent (e.g. a header has a line break in it), and std::logic_error
    // if the final response was sent already: any number of interim (1xx) responses may come before it, but one
    // request gets one final response.
    //
    // An interim response (e.g. status::EARLY_HINTS, with Link fields) is sent at once, flushing the connection over
    // HTTP/1.1, and keeps the headers, which the final response then sends too (as RFC 8297 expects of Early Hints).
    // It isn't sent to an HTTP/1.0 client, which doesn't know them (RFC 9110, section 15.2): send() then returns a
    // writer that drops what's written. status::SWITCHING_PROTOCOLS can't be sent (std::system_error, with
    // std::errc::not_supported): it would tell an HTTP/1.1 client the connection had switched, which only switching it
    // does (as what speaks the other protocol does, e.g. WebSockets; see switched()), and HTTP/2 has no 101 (RFC 9113,
    // section 8.6). A request that expects 100 Continue gets one from the server, when the handler first reads its
    // body, unless the handler sent its own 100 or the final response before (another interim response doesn't count;
    // see expect_continue()).
    //
    // NOTE: when the encoder finishes without a coroutine (e.g. the head fits in the connection's buffer), so does
    //       send(), which then throws when it's called, rather than when it's awaited.
    coro::task<io::writer*> send(status status_code, std::size_t content_length);

    // send sends the response's status and headers without a Content-Length, for a body whose length isn't known
    // beforehand, and returns the writer for it: the body is as long as what's written to it. It's sent chunked to an
    // HTTP/1.1 client, and finish() (which the server calls once the handler returns) ends it; to an HTTP/1.0 client,
    // closing the connection ends it; and HTTP/2 frames it as it goes, ended by finish(). Otherwise, as
    // send(status_code, content_length).
    coro::task<io::writer*> send(status status_code);

    // finish ends the body of a response sent without a Content-Length, if it's chunked (see send(status)), or an
    // HTTP/2 response's stream, sending the trailers (see trailers()). It does nothing otherwise, or if the body was
    // ended already. It fails with std::errc::invalid_argument, having sent nothing, if a trailer can't be sent.
    coro::task<io::result> finish();

    // body_remaining is how much of the body the handler has yet to write.
    [[nodiscard]] std::size_t body_remaining() const noexcept { return body.remaining(); }

    // status_code is the status that was sent last, or status::NONE if none has been yet.
    [[nodiscard]] status status_code() const noexcept { return resp->status_code; }

    // sent is whether the final (not 1xx) response has been sent.
    [[nodiscard]] bool sent() const noexcept { return final_sent; }

    // switched is whether the response was a 101 (Switching Protocols), after which the connection was the handler's,
    // speaking another protocol (e.g. WebSocket), until the handler returned. It counts as sent too.
    [[nodiscard]] bool switched() const noexcept { return tunneled; }

    // expect_continue is the server's: the request expects 100 (Continue) before its client sends the body (Expect:
    // 100-continue, RFC 9110, section 10.1.1), which is to come. It returns the callback for the body's first read to
    // await (see io::start_callback), which sends the 100 then (just the status, without the response's headers),
    // unless the handler has sent its own 100, or the final response, by then: another interim response (e.g. Early
    // Hints) doesn't stop it, as in Go's server, so a handler may send a 103 and then read the body. Over HTTP/1.1, a
    // final response sent before the body was asked for says the connection closes after it (Connection: close), as
    // the client may never send the body (see continue_unmet()).
    [[nodiscard]] io::start_callback expect_continue() noexcept;

    // continue_unmet is whether the request expected 100 Continue, and its body hasn't been asked for: the final
    // response was sent first, and the handler hasn't read it since. The server then closes the connection after the
    // response, rather than waiting for a body that may never come, to discard it.
    [[nodiscard]] bool continue_unmet() const noexcept { return continuation == continue_state::unmet; }

private:
    friend coro::task<std::expected<detail::tunnel*, std::error_code>> detail::switch_protocols(response_writer& rw,
                                                                                                fields_view fields);

    // continue_state is how a request's expectation of 100 Continue stands.
    enum class continue_state : std::uint8_t
    {
        none,  // there was none, or it's been met: a 100 was sent, or the body's being read regardless
        owed,  // no 100 nor final response has been sent: the body's first read sends a 100
        unmet, // the final response was sent first, and the body hasn't been read
    };

    // body_starts is expect_continue()'s callback (its context the response_writer), which has send_continue() send
    // the 100, if it's owed.
    static coro::task<std::error_code> body_starts(void* context);
    coro::task<std::error_code>        send_continue();

    // send_interim finishes sending an interim response over HTTP/1.1, flushing it once encoding (which may need a
    // coroutine) does.
    coro::task<io::writer*> send_interim(coro::task<response_encoder_result> encoding);

    // switch_to does detail::switch_protocols()'s part once it has checked what it can without the connection.
    coro::task<std::expected<detail::tunnel*, std::error_code>> switch_to(fields_view fields);

    // how a response's body is framed: by its Content-Length, chunks, or closing the connection.
    enum class framing : std::uint8_t
    {
        length,
        chunked,
        close,
    };

    // start starts sending the response, framing its body as framing says (with a length of body_length).
    coro::task<io::writer*> start(status status_code, framing how, std::size_t body_length);

    // finish_send finishes sending the response, once encoding (which needed a coroutine) does.
    coro::task<io::writer*>
    finish_send(coro::task<response_encoder_result> encoding, framing how, std::size_t body_length);

    // take_encoding takes the encoder's result, returning the body's writer, or throwing if it failed.
    io::writer* take_encoding(const response_encoder_result& res, framing how, std::size_t body_length);

    // http11_backend frames the response for HTTP/1.x: its head through encode, and its body by its length, chunks,
    // or closing the connection.
    struct http11_backend
    {
        io::writer*                writer;
        response_encoder           encode;
        const std::stop_token*     close_after;
        const detail::switch_hook* switching;
        http11::chunked_writer     chunked{nullptr};
        bool                       chunked_open = false;
    };

    // http2_backend sends the response on an HTTP/2 stream, which frames its body itself.
    struct http2_backend
    {
        detail::http2_stream*           stream;
        const detail::http2_stream_ops* ops;
    };

    server_response*                            resp;
    std::variant<http11_backend, http2_backend> backend;
    io::limit_writer                            body;
    bool                                        final_sent   = false;
    bool                                        tunneled     = false; // see switched()
    continue_state                              continuation = continue_state::none;
};

}
