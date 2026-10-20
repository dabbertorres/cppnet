#include "http/http11.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <typeinfo>
#include <utility>
#include <vector>

#include "coro/task.hpp"
#include "http/chunked_reader.hpp"
#include "http/chunked_writer.hpp"
#include "http/headers.hpp"
#include "http/http.hpp"
#include "http/request.hpp"
#include "http/request_target.hpp"
#include "http/response.hpp"
#include "io/buffered_reader.hpp"
#include "io/buffered_writer.hpp"
#include "io/io.hpp"
#include "io/limit_reader.hpp"
#include "io/reader.hpp"
#include "io/util.hpp"
#include "io/writer.hpp"
#include "url.hpp"
#include "util/string_util.hpp"

namespace
{

using namespace std::string_view_literals;

using net::coro::task;
using net::http::client_request;
using net::http::client_response;
using net::http::headers;
using net::http::parse_method;
using net::http::parse_request_target;
using net::http::parse_status;
using net::http::protocol_version;
using net::http::request_method;
using net::http::server_request;
using net::http::status;
using net::io::buffered_reader;

std::error_code invalid() noexcept { return std::make_error_code(std::errc::invalid_argument); }

constexpr bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }

// head_reader reads the lines of a message's head (its start line and fields), which are limited in size altogether.
//
// A line is usually all buffered already, so it's taken from the buffer as it is (buffered_line()): only one that isn't
// is read (read_line()), with a coroutine, and copied.
struct head_reader
{
    buffered_reader* reader;
    std::size_t      remaining;
    std::string      line{}; // the last line read_line() read

    // buffered_line takes the next line (without its CRLF) from what's buffered, if it's all there, and within the
    // limit. It views the reader's buffer, so it's only valid until the reader next reads.
    std::optional<std::string_view> buffered_line() noexcept
    {
        const auto       buffered = reader->buffered();
        std::string_view chars{reinterpret_cast<const char*>(buffered.data()), std::min(buffered.size(), remaining)};

        const auto end = chars.find("\r\n"sv);
        if (end == std::string_view::npos) return std::nullopt;

        reader->consume(end + 2);
        remaining -= end + 2;
        chars.remove_suffix(chars.size() - end);
        return chars;
    }

    // read_line reads the next line (without its CRLF) into line.
    task<std::error_code> read_line() noexcept
    {
        auto read = co_await net::io::read_terminated_line(reader, remaining);
        if (!read.has_value()) co_return read.error();

        line = std::move(read.value());
        remaining -= line.size() + 2;
        co_return std::error_code{};
    }
};

// parse_http_version parses exactly "HTTP/" DIGIT "." DIGIT (RFC 9112, section 2.3).
std::optional<protocol_version> parse_http_version(std::string_view str) noexcept
{
    if (str.size() != 8 || !str.starts_with("HTTP/"sv) || !is_digit(str[5]) || str[6] != '.' || !is_digit(str[7]))
        return std::nullopt;

    return protocol_version{
        .major = static_cast<std::uint32_t>(str[5] - '0'),
        .minor = static_cast<std::uint32_t>(str[7] - '0'),
    };
}

// parse_status_line parses "HTTP/1.1 200 OK". The reason (and the space before it) may be missing: it's ignored anyway.
// NOLINTNEXTLINE(bugprone-exception-escape): substr's positions are in range (after the size check).
std::error_code parse_status_line(std::string_view line, client_response& resp) noexcept
{
    auto version = parse_http_version(line.substr(0, line.find(' ')));
    if (!version.has_value() || line.size() < 12 || line[8] != ' ') return invalid();

    auto code = line.substr(9, 3);
    if (!std::ranges::all_of(code, is_digit) || code[0] < '1' || code[0] > '5') return invalid();
    if (line.size() > 12 && line[12] != ' ') return invalid();
    if (!net::http::is_valid_field_value(line.substr(12))) return invalid();

    resp.version     = *version;
    resp.status_code = parse_status(code);
    return {};
}

// parse_request_line parses "GET /path HTTP/1.1" (RFC 9112, section 3), with exactly one space between each part.
// NOLINTNEXTLINE(bugprone-exception-escape): substr's positions are in range.
std::error_code parse_request_line(std::string_view line, server_request& req) noexcept
{
    auto method_end = line.find(' ');
    if (method_end == std::string_view::npos) return invalid();

    auto target_end = line.find(' ', method_end + 1);
    if (target_end == std::string_view::npos) return invalid();

    auto method  = line.substr(0, method_end);
    auto target  = line.substr(method_end + 1, target_end - method_end - 1);
    auto version = parse_http_version(line.substr(target_end + 1));

    if (!net::http::is_valid_field_name(method) || !version.has_value()) return invalid();

    // methods are case-sensitive, and one that's well-formed, but unknown, isn't implemented (RFC 9110, section 9.1).
    req.method = parse_method(method);
    if (req.method == request_method::NONE || method_string(req.method) != method)
        return std::make_error_code(std::errc::not_supported);

    req.version = *version;
    return parse_request_target(req.method, target, req.target);
}

// add_field_line adds a field line to fields, returning whether it was the empty line that ends them. count is how many
// came before it.
std::expected<bool, std::error_code> add_field_line(std::string_view line, std::size_t count, headers& fields)
{
    if (line.empty()) return true;
    if (count == net::http::http11::max_fields)
        return std::unexpected(std::make_error_code(std::errc::value_too_large));

    auto field = net::http::parse_field_line(line);
    if (!field.has_value()) return std::unexpected(invalid());

    // NOTE: each line is kept whole: splitting on commas would break fields that aren't lists (e.g. User-Agent).
    fields.add(field->first, field->second);
    return false;
}

// parse_fields reads field lines up to the empty line that ends them.
task<std::error_code> parse_fields(head_reader& head, headers& fields) noexcept
{
    for (std::size_t count = 0;; ++count)
    {
        auto line = head.buffered_line();
        if (!line.has_value())
        {
            if (auto err = co_await head.read_line(); err) co_return err;
            line = head.line;
        }

        auto added = add_field_line(*line, count, fields);
        if (!added.has_value()) co_return added.error();
        if (*added) co_return std::error_code{};
    }
}

// count_values counts the field lines of key.
std::size_t count_values(const headers& fields, std::string_view key)
{
    auto all = fields.get_all(key);
    return all.has_value() ? static_cast<std::size_t>(std::ranges::distance(*all)) : 0;
}

// parse_host_header checks the Host header (RFC 9112, section 3.2), and sets the request's host.
std::error_code parse_host_header(server_request& req)
{
    const auto lines = count_values(req.headers, "Host"sv);

    // HTTP/1.1 requires exactly one, and HTTP/1.0 doesn't know it, but mustn't repeat it either.
    if (lines > 1 || (lines == 0 && req.version.major == 1 && req.version.minor >= 1)) return invalid();

    req.host.clear();
    if (lines == 1)
    {
        auto value = req.headers.get("Host"sv).value_or(""sv);

        // an empty value is allowed, for targets without a host.
        if (!value.empty())
        {
            auto parsed = net::http::parse_host(value);
            if (!parsed.has_value()) return invalid();
            req.host.assign(parsed->host);
        }
    }

    // a target with a host overrides the Host header (RFC 9112, section 3.2.2).
    if (!req.target.host.empty()) req.host.assign(req.target.host);
    return {};
}

// reuse_or_make sets body to a T made from args, reusing body's memory if it's already one.
template<typename T, typename... Args>
void reuse_or_make(std::unique_ptr<net::io::reader>& body, Args&&... args)
{
    const auto* existing = body.get();
    if (existing != nullptr && typeid(*existing) == typeid(T))
        *static_cast<T*>(body.get()) = T(std::forward<Args>(args)...);
    else body = std::make_unique<T>(std::forward<Args>(args)...);
}

// set_request_body decides how long the request's body is (RFC 9112, section 6.3), and sets its body to read only that
// much of reader. The rules are strict where requests are ambiguous, as an ambiguity between this server and anything
// in front of it is what request smuggling exploits.
std::error_code set_request_body(buffered_reader* reader, server_request& req)
{
    if (req.headers.get_all("Transfer-Encoding"sv).has_value())
    {
        // HTTP/1.0 has no transfer codings, and a length from both would be ambiguous.
        const bool http10 = req.version.major == 1 && req.version.minor == 0;
        if (http10 || req.headers.get_all("Content-Length"sv).has_value()) return invalid();

        // chunked must be last, and only applied once (RFC 9112, section 6.1).
        auto codings  = req.headers.get_list("Transfer-Encoding"sv);
        auto chunked  = [](std::string_view coding) { return net::util::equal_ignore_case(coding, "chunked"sv); };
        auto how_many = std::ranges::count_if(codings, chunked);
        if (codings.empty() || !chunked(codings.back()) || how_many > 1) return invalid();

        // and nothing else is implemented.
        if (codings.size() > 1) return std::make_error_code(std::errc::not_supported);

        reuse_or_make<net::http::http11::chunked_reader>(req.body, reader);
        return {};
    }

    std::size_t length = 0;
    if (req.headers.get_all("Content-Length"sv).has_value())
    {
        auto parsed = req.headers.get_content_length();
        if (!parsed.has_value()) return invalid();
        length = *parsed;
    }

    // NOTE: a body that ends before its length fails (std::errc::connection_aborted), so it isn't taken for all of it.
    reuse_or_make<net::io::limit_reader>(req.body, static_cast<net::io::reader*>(reader), length);
    return {};
}

// response_framing is how a response's body ends (RFC 9112, section 6.3).
enum class response_framing : std::uint8_t
{
    none,      // there's no body.
    chunked,   // with the last chunk.
    length,    // after Content-Length bytes.
    connection // with the connection: if nothing else says, or after a 101 (when it's the new protocol's).
};

std::expected<response_framing, std::error_code> framing_of(const client_response& resp, request_method method)
{
    if (resp.status_code == status::SWITCHING_PROTOCOLS) return response_framing::connection;

    if (method == request_method::HEAD || net::http::status_is_information(resp.status_code)
        || resp.status_code == status::NO_CONTENT || resp.status_code == status::NOT_MODIFIED)
        return response_framing::none;

    // if chunked isn't last, the body ends with the connection.
    if (resp.headers.get_all("Transfer-Encoding"sv).has_value())
        return resp.headers.is_chunked() ? response_framing::chunked : response_framing::connection;

    if (resp.headers.get_all("Content-Length"sv).has_value())
    {
        if (!resp.headers.get_content_length().has_value()) return std::unexpected(invalid());
        return response_framing::length;
    }

    return response_framing::connection;
}

// make_response_body wraps reader (which it borrows) to read only the response's body.
std::expected<std::unique_ptr<net::io::reader>, std::error_code>
make_response_body(buffered_reader* reader, const client_response& resp, request_method method)
{
    auto framing = framing_of(resp, method);
    if (!framing.has_value()) return std::unexpected(framing.error());

    auto* parent = static_cast<net::io::reader*>(reader);

    switch (*framing)
    {
    case response_framing::none: return std::make_unique<net::io::limit_reader>(parent, 0);
    case response_framing::chunked: return std::make_unique<net::http::http11::chunked_reader>(reader);
    case response_framing::length:
        // NOTE: a body that ends before its length fails (std::errc::connection_aborted), so it isn't taken for all of
        // it (nor its connection reused).
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access): only chosen when there's a valid Content-Length.
        return std::make_unique<net::io::limit_reader>(parent, *resp.headers.get_content_length());
    case response_framing::connection:
        // the connection's end is the body's: it can only be cut short as far as the connection can tell (e.g. TLS's
        // stream_truncated).
        return std::make_unique<net::io::limit_reader>(parent,
                                                       std::numeric_limits<std::size_t>::max(),
                                                       net::io::limit_reader::mode::up_to);
    }

    std::unreachable();
}

// owning_body reads a body from a reader that it also owns.
class owning_body final : public net::io::reader
{
public:
    owning_body(std::unique_ptr<net::io::reader> owned, std::unique_ptr<net::io::reader> body) noexcept
        : owned{std::move(owned)}
        , body{std::move(body)}
    {}

    task<net::io::result> read(std::span<std::byte> data) override { return body->read(data); }

    using net::io::reader::read;

    [[nodiscard]] net::io::handle native_handle() const noexcept override { return body->native_handle(); }

private:
    // NOTE: declared first, so it's destroyed last: body reads from it.
    std::unique_ptr<net::io::reader> owned;
    std::unique_ptr<net::io::reader> body;
};

// request_framing is how a request's body is sent.
struct request_framing
{
    enum class kind : std::uint8_t
    {
        none,
        length,
        chunked,
    };

    kind        how    = kind::none;
    std::size_t length = 0;

    // A field to add for it, if the request doesn't have it.
    std::string_view field;
};

protocol_version effective_version(const client_request& req) noexcept
{
    // an unset version is HTTP/1.0.
    if (req.version == protocol_version{}) return {.major = 1, .minor = 0};
    return req.version;
}

// frame_request decides how a request's body is sent (RFC 9112, section 6), from its Content-Length or
// Transfer-Encoding if it has them.
std::expected<request_framing, std::error_code> frame_request(const client_request& req)
{
    using enum request_framing::kind;

    const auto version = effective_version(req);
    if (version.major != 1 || version.minor > 1) return std::unexpected(std::make_error_code(std::errc::not_supported));

    const bool has_te = req.headers.get_all("Transfer-Encoding"sv).has_value();
    const bool has_cl = req.headers.get_all("Content-Length"sv).has_value();

    if (has_te)
    {
        // only chunked is implemented, and HTTP/1.0 has no transfer codings.
        auto codings = req.headers.get_list("Transfer-Encoding"sv);
        if (has_cl || version.minor == 0) return std::unexpected(invalid());
        if (codings.size() != 1 || !net::util::equal_ignore_case(codings.front(), "chunked"sv))
            return std::unexpected(std::make_error_code(std::errc::not_supported));

        return request_framing{.how = chunked, .length = 0, .field = {}};
    }

    if (has_cl)
    {
        auto content_length = req.headers.get_content_length();
        if (!content_length.has_value() || (req.body == nullptr && *content_length != 0))
            return std::unexpected(invalid());

        return request_framing{.how = length, .length = *content_length, .field = {}};
    }

    if (req.body != nullptr)
    {
        // a body of unknown length needs chunked.
        if (version.minor == 0) return std::unexpected(invalid());
        return request_framing{.how = chunked, .length = 0, .field = "Transfer-Encoding: chunked\r\n"sv};
    }

    // methods that expect content say there's none (RFC 9110, section 8.6).
    const bool expects_content =
        req.method == request_method::POST || req.method == request_method::PUT || req.method == request_method::PATCH;
    return request_framing{.how = none, .length = 0, .field = expects_content ? "Content-Length: 0\r\n"sv : ""sv};
}

task<std::error_code> write_full(net::io::writer& out, std::span<const std::byte> data)
{
    while (!data.empty())
    {
        auto res = co_await out.write(data);
        if (res.err) co_return res.err;
        if (res.count == 0) co_return make_error_code(net::io::errc::no_progress);

        data = data.subspan(res.count);
    }

    co_return std::error_code{};
}

// max_start_line_overhead is enough for a request or status line, apart from the request's target: e.g. the method,
// version, status code and reason phrase, and the line's end and the head's.
constexpr std::size_t max_start_line_overhead = 128;

// body_buffer is how much of a body is read at a time.
constexpr std::size_t body_buffer = std::size_t{16} * 1'024;

// copy_length copies exactly length bytes from body, which mustn't end before then.
task<std::error_code> copy_length(net::io::reader& body, net::io::writer& out, std::size_t length)
{
    std::vector<std::byte> buf(std::min(length, body_buffer));

    while (length > 0)
    {
        auto res = co_await body.read(std::span{buf}.first(std::min(buf.size(), length)));

        if (auto err = co_await write_full(out, std::span{buf}.first(res.count)); err) co_return err;
        length -= res.count;

        if (length == 0) break;

        // a body shorter than its Content-Length is the caller's mistake.
        if (res.err == net::io::errc::end_of_stream) co_return invalid();
        if (res.err) co_return res.err;
        if (res.count == 0) co_return make_error_code(net::io::errc::no_progress);
    }

    co_return std::error_code{};
}

// copy_chunked copies body, until it ends, as chunks.
task<std::error_code> copy_chunked(net::io::reader& body, net::io::writer& out)
{
    net::http::http11::chunked_writer chunked{&out};
    std::vector<std::byte>            buf(body_buffer);

    while (true)
    {
        auto res = co_await body.read(buf);

        if (res.count > 0)
        {
            if (auto written = co_await chunked.write(std::span{buf}.first(res.count)); written.err)
                co_return written.err;
        }

        if (res.err == net::io::errc::end_of_stream) break;
        if (res.err) co_return res.err;
        if (res.count == 0) co_return make_error_code(net::io::errc::no_progress);
    }

    co_return (co_await chunked.finish()).err;
}

bool is_default_port(std::string_view scheme, std::string_view port) noexcept
{
    return (port == "80"sv && (scheme.empty() || net::util::equal_ignore_case(scheme, "http"sv)))
        || (port == "443"sv && net::util::equal_ignore_case(scheme, "https"sv));
}

// fields_size is how long append_fields makes the field lines for fields.
std::size_t fields_size(const headers& fields) noexcept
{
    std::size_t size = 0;
    for (const auto& [name, values] : fields)
    {
        for (const auto& value : values) size += name.size() + value.size() + 4;
    }

    return size;
}

// span_appender appends to a span, as std::string::append does to a string: the span must be long enough.
struct span_appender
{
    std::span<std::byte> out;
    std::size_t          size = 0;

    span_appender& append(std::string_view str) noexcept
    {
        std::ranges::copy(std::as_bytes(std::span{str}), out.subspan(size).begin());
        size += str.size();
        return *this;
    }
};

// append_fields appends a field line for each of the values in fields: joining a field's values with commas would
// change fields that aren't lists (e.g. Set-Cookie).
void append_fields(auto& out, const headers& fields)
{
    for (const auto& [name, values] : fields)
    {
        for (const auto& value : values) out.append(name).append(": "sv).append(value).append("\r\n"sv);
    }
}

// status_line_size is how long append_status_head makes the status line (with its CRLF).
std::size_t status_line_size(const net::http::server_response& resp) noexcept
{
    // "HTTP/1.1 200 " and the reason, then CRLF.
    return 13 + net::http::reason_phrase(resp.status_code).size() + 2;
}

// append_status_head appends a response's head: its status line, field lines, and the empty line after them. The status
// code must be from 100 to 599.
void append_status_head(auto& out, const net::http::server_response& resp)
{
    std::array<char, 3> code{};
    std::to_chars(code.begin(), code.end(), static_cast<std::uint16_t>(resp.status_code));

    const std::array<char, 4> version{static_cast<char>(resp.version.major + '0'),
                                      '.',
                                      static_cast<char>(resp.version.minor + '0'),
                                      ' '};

    out.append("HTTP/"sv)
        .append(std::string_view{version.data(), version.size()})
        .append(std::string_view{code.data(), code.size()})
        .append(" "sv)
        .append(net::http::reason_phrase(resp.status_code))
        .append("\r\n"sv);

    append_fields(out, resp.headers);
    out.append("\r\n"sv);
}

// write_status_head writes a response's head that didn't fit in the writer's buffer (see response_encode).
task<net::http::response_encoder_result> write_status_head(net::io::writer*                  writer,
                                                           const net::http::server_response& resp) noexcept
{
    try
    {
        // NOTE: written all at once, rather than a write (and so, a coroutine) for each part of it.
        std::string head;
        head.reserve(status_line_size(resp) + fields_size(resp.headers) + 2);
        append_status_head(head, resp);

        if (auto err = co_await write_full(*writer, std::as_bytes(std::span{head})); err)
            co_return std::unexpected(err);
        co_return writer;
    }
    catch (const std::bad_alloc&)
    {
        co_return std::unexpected(std::make_error_code(std::errc::not_enough_memory));
    }
}

// head_state is how far decoding a request's head has got.
struct head_state
{
    bool        started     = false; // whether the request line has been parsed
    int         empty_lines = 0;
    std::size_t fields      = 0;
};

// take_head_line parses the next line of a request's head, returning whether it was the last (the empty line after the
// fields).
std::expected<bool, std::error_code> take_head_line(std::string_view line, head_state& state, server_request& req)
{
    if (!state.started)
    {
        // an empty line before the request line is ignored (RFC 9112, section 2.2), e.g. a CRLF after a POST's body.
        if (line.empty() && ++state.empty_lines < 2) return false;

        if (auto err = parse_request_line(line, req); err) return std::unexpected(err);
        state.started = true;
        return false;
    }

    return add_field_line(line, state.fields++, req.headers);
}

// head_is_buffered is whether the reader's buffer holds a whole head (up to the empty line that ends it), within
// max_header_bytes: then decoding it doesn't need to read.
bool head_is_buffered(const buffered_reader* reader, std::size_t max_header_bytes) noexcept
{
    const auto       buffered = reader->buffered();
    std::string_view chars{reinterpret_cast<const char*>(buffered.data()), std::min(buffered.size(), max_header_bytes)};

    // NOTE: every line ends with CRLF (see head_reader), so this is the end of a line and then an empty line: the
    //       first empty line after the request line is at or before it (and one before the request line is skipped).
    return chars.contains("\r\n\r\n"sv);
}

// decode_buffered decodes a request whose head is buffered (see head_is_buffered), without a coroutine.
std::error_code decode_buffered(buffered_reader* reader, std::size_t max_header_bytes, server_request& req) noexcept
{
    try
    {
        head_reader head{.reader = reader, .remaining = max_header_bytes};
        head_state  state;

        while (true)
        {
            // NOTE: the lines are all buffered, so buffered_line() only fails if the head ends in a way it can't.
            auto line = head.buffered_line();
            if (!line.has_value()) return invalid();

            auto last = take_head_line(*line, state, req);
            if (!last.has_value()) return last.error();
            if (*last) break;
        }

        if (auto err = parse_host_header(req); err) return err;
        return set_request_body(reader, req);
    }
    catch (const std::bad_alloc&)
    {
        return std::make_error_code(std::errc::not_enough_memory);
    }
}

// decode_reading decodes a request whose head isn't all buffered yet, reading the rest. Each line is still parsed as it
// comes (so a malformed one fails without waiting for the rest), from the reader's buffer: it's refilled until the line
// is all there, rather than the line read into a string, unless the buffer can't hold it.
task<std::error_code>
decode_reading(buffered_reader* reader, std::size_t max_header_bytes, server_request& req) noexcept
{
    try
    {
        head_reader head{.reader = reader, .remaining = max_header_bytes};
        head_state  state;

        while (true)
        {
            std::string_view line;
            if (auto buffered = head.buffered_line(); buffered.has_value())
            {
                line = *buffered;
            }
            else if (reader->size() >= head.remaining)
            {
                // the head's longer than it may be.
                co_return std::make_error_code(std::errc::value_too_large);
            }
            else if (reader->size() == reader->capacity())
            {
                // the line's longer than the buffer.
                if (auto err = co_await head.read_line(); err) co_return err;
                line = head.line;
            }
            else
            {
                if (auto err = co_await reader->fill(); err) co_return err;
                continue;
            }

            auto last = take_head_line(line, state, req);
            if (!last.has_value()) co_return last.error();
            if (*last) break;
        }

        if (auto err = parse_host_header(req); err) co_return err;
        co_return set_request_body(reader, req);
    }
    catch (const std::bad_alloc&)
    {
        co_return std::make_error_code(std::errc::not_enough_memory);
    }
}
}

namespace net::http::http11
{

using coro::task;

std::error_code check_request(const client_request& req) noexcept
{
    try
    {
        if (req.method == request_method::NONE || !valid_fields(req.headers)) return invalid();

        auto framing = frame_request(req);
        if (!framing.has_value()) return framing.error();

        return {};
    }
    catch (const std::bad_alloc&)
    {
        return std::make_error_code(std::errc::not_enough_memory);
    }
}

task<request_encoder_result> request_encode(io::writer* writer, const client_request& req) noexcept
{
    if (auto err = check_request(req); err) co_return std::unexpected(err);

    // NOTE: can't fail, having been checked.
    const auto framing = *frame_request(req);

    // NOTE: the head is written all at once, rather than a write (and so, a coroutine) for each part of it.
    // the target is written in origin-form ("/path?query"), encoded so that nothing in the url can end the line.
    const auto target = req.uri.origin_form();

    std::string head;
    head.reserve(max_start_line_overhead + target.size() + req.uri.host.size() + req.uri.port.size()
                 + framing.field.size() + fields_size(req.headers));

    // NOTE: unlike the status line, nothing follows the version.
    const auto version = effective_version(req);
    head.append(method_string(req.method))
        .append(" "sv)
        .append(target)
        .append(" HTTP/"sv)
        .append({static_cast<char>(version.major + '0'), '.', static_cast<char>(version.minor + '0')})
        .append("\r\n"sv);

    // HTTP/1.1 requires a Host header.
    if (!req.headers.get("Host"sv).has_value() && !req.uri.host.empty())
    {
        // an IPv6 address is bracketed, so its colons aren't mistaken for the port's.
        const bool bracketed = req.uri.host.contains(':');
        head.append(bracketed ? "Host: ["sv : "Host: "sv).append(req.uri.host).append(bracketed ? "]"sv : ""sv);
        if (!req.uri.port.empty() && !is_default_port(req.uri.scheme, req.uri.port))
            head.append(":"sv).append(req.uri.port);
        head.append("\r\n"sv);
    }

    head.append(framing.field);
    append_fields(head, req.headers);
    head.append("\r\n"sv);

    if (auto err = co_await write_full(*writer, std::as_bytes(std::span{head})); err) co_return std::unexpected(err);

    try
    {
        std::error_code err;

        switch (framing.how)
        {
        case request_framing::kind::none: break;
        case request_framing::kind::length:
            if (framing.length > 0) err = co_await copy_length(*req.body, *writer, framing.length);
            break;
        case request_framing::kind::chunked:
            // a chunked request without a body (only if it asked for chunked) is just the last chunk.
            if (req.body != nullptr)
            {
                err = co_await copy_chunked(*req.body, *writer);
            }
            else
            {
                err = co_await write_full(*writer, std::as_bytes(std::span{"0\r\n\r\n"sv}));
            }
            break;
        }

        if (err) co_return std::unexpected(err);
    }
    catch (const std::bad_alloc&)
    {
        co_return std::unexpected(std::make_error_code(std::errc::not_enough_memory));
    }

    co_return writer;
}

task<response_encoder_result> response_encode(io::writer* writer, const server_response& resp) noexcept
{
    using result = response_encoder_result;

    if (writer == nullptr || !valid_fields(resp.headers)) return coro::ready_task<result>(std::unexpected(invalid()));

    // a status code is three digits, from 100 to 599 (RFC 9110, section 15).
    const auto code = static_cast<std::uint16_t>(resp.status_code);
    if (code < 100 || code > 599) return coro::ready_task<result>(std::unexpected(invalid()));

    // usually, it's a connection's buffered writer, with room for the head: then it's formatted straight into the
    // buffer, without a coroutine (see coro::ready_task).
    if (auto* buffered = dynamic_cast<io::buffered_writer*>(writer); buffered != nullptr)
    {
        try
        {
            const auto size = status_line_size(resp) + fields_size(resp.headers) + 2;
            if (auto out = buffered->prepare(size); !out.empty())
            {
                span_appender appender{.out = out};
                append_status_head(appender, resp);
                return coro::ready_task<result>(writer);
            }
        }
        catch (const std::bad_alloc&)
        {
            return coro::ready_task<result>(std::unexpected(std::make_error_code(std::errc::not_enough_memory)));
        }
    }

    return write_status_head(writer, resp);
}

task<request_decoder_result> request_decode(io::buffered_reader* reader, std::size_t max_header_bytes) noexcept
{
    try
    {
        server_request req;
        if (auto err = co_await request_decode(reader, max_header_bytes, req); err) co_return std::unexpected(err);
        co_return req;
    }
    catch (const std::bad_alloc&)
    {
        co_return std::unexpected(std::make_error_code(std::errc::not_enough_memory));
    }
}

task<std::error_code>
request_decode(io::buffered_reader* reader, std::size_t max_header_bytes, server_request& req) noexcept
{
    req.clear();

    // e.g. for pipelined requests, which are read together.
    if (head_is_buffered(reader, max_header_bytes))
        return coro::ready_task<std::error_code>(decode_buffered(reader, max_header_bytes, req));

    return decode_reading(reader, max_header_bytes, req);
}

void call_at_body_end(const server_request& req, io::end_callback callback) noexcept
{
    // NOTE: the bodies request_decode() makes (see set_request_body()), told apart as reuse_or_make() does, rather
    //       than with a dynamic_cast for each request.
    auto* body = req.body.get();
    if (body == nullptr) return;

    // NOLINTBEGIN(cppcoreguidelines-pro-type-static-cast-downcast): the type is checked first.
    if (typeid(*body) == typeid(io::limit_reader)) static_cast<io::limit_reader*>(body)->call_at_end(callback);
    else if (typeid(*body) == typeid(chunked_reader)) static_cast<chunked_reader*>(body)->call_at_end(callback);
    // NOLINTEND(cppcoreguidelines-pro-type-static-cast-downcast)
}

void call_at_body_start(const server_request& req, io::start_callback callback) noexcept
{
    // NOTE: as call_at_body_end().
    auto* body = req.body.get();
    if (body == nullptr) return;

    // NOLINTBEGIN(cppcoreguidelines-pro-type-static-cast-downcast): the type is checked first.
    if (typeid(*body) == typeid(io::limit_reader)) static_cast<io::limit_reader*>(body)->call_at_start(callback);
    else if (typeid(*body) == typeid(chunked_reader)) static_cast<chunked_reader*>(body)->call_at_start(callback);
    // NOLINTEND(cppcoreguidelines-pro-type-static-cast-downcast)
}

coro::task<response_decoder_result>
response_decode(io::buffered_reader* reader, request_method method, std::size_t max_header_bytes) noexcept
{
    try
    {
        client_response resp;
        head_reader     head{.reader = reader, .remaining = max_header_bytes};

        // interim (1xx) responses, e.g. 100 Continue, come before the final response, and are skipped (other than 101,
        // which is the last thing on the connection in HTTP/1.1).
        do
        {
            resp.headers = {};

            auto line = head.buffered_line();
            if (!line.has_value())
            {
                if (auto err = co_await head.read_line(); err) co_return std::unexpected(err);
                line = head.line;
            }

            if (auto err = parse_status_line(*line, resp); err) co_return std::unexpected(err);
            if (auto err = co_await parse_fields(head, resp.headers); err) co_return std::unexpected(err);
        }
        while (status_is_information(resp.status_code) && resp.status_code != status::SWITCHING_PROTOCOLS);

        auto body = make_response_body(reader, resp, method);
        if (!body.has_value()) co_return std::unexpected(body.error());
        resp.body = std::move(*body);

        co_return resp;
    }
    catch (const std::bad_alloc&)
    {
        co_return std::unexpected(std::make_error_code(std::errc::not_enough_memory));
    }
}

coro::task<response_decoder_result> response_decode(std::unique_ptr<io::buffered_reader> reader,
                                                    request_method                       method,
                                                    std::size_t                          max_header_bytes) noexcept
{
    auto resp = co_await response_decode(reader.get(), method, max_header_bytes);
    if (!resp.has_value()) co_return resp;

    try
    {
        resp->body = std::make_unique<owning_body>(std::move(reader), std::move(resp->body));
    }
    catch (const std::bad_alloc&)
    {
        co_return std::unexpected(std::make_error_code(std::errc::not_enough_memory));
    }

    co_return resp;
}

bool connection_reusable(const client_response& resp, request_method method) noexcept
{
    try
    {
        // HTTP/1.0 connections close by default, and are kept only by a keep-alive extension that isn't implemented.
        if (resp.version != protocol_version{.major = 1, .minor = 1}) return false;

        auto connection = resp.headers.get_list("Connection"sv);
        if (std::ranges::any_of(connection, [](auto option) { return util::equal_ignore_case(option, "close"sv); }))
            return false;

        auto framing = framing_of(resp, method);
        return framing.has_value() && *framing != response_framing::connection;
    }
    catch (const std::bad_alloc&)
    {
        return false;
    }
}

}
