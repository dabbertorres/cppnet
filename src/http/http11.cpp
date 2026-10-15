#include "http/http11.hpp"

#include <algorithm>
#include <array>
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
#include <unordered_map>
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
using net::http::parse_status;
using net::http::protocol_version;
using net::http::parse_request_target;
using net::http::request_method;
using net::http::server_request;
using net::http::status;
using net::io::buffered_reader;

std::error_condition invalid() noexcept { return std::make_error_condition(std::errc::invalid_argument); }

constexpr bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }

// head_reader reads the lines of a message's head (its start line and fields), which are limited in size altogether.
struct head_reader
{
    buffered_reader* reader;
    std::size_t      remaining;

    task<net::io::readline_result> next_line() noexcept
    {
        auto line = co_await net::io::read_terminated_line(reader, remaining);
        if (line.has_value()) remaining -= line.to_value().size() + 2;
        co_return line;
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
std::error_condition parse_status_line(std::string_view line, client_response& resp) noexcept
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
std::error_condition parse_request_line(std::string_view line, server_request& req) noexcept
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
        return std::make_error_condition(std::errc::not_supported);

    req.version = *version;

    auto parsed = parse_request_target(req.method, target);
    if (!parsed.has_value()) return parsed.error();
    req.target = std::move(*parsed);

    return {};
}

// parse_fields reads field lines up to the empty line that ends them.
task<std::error_condition> parse_fields(head_reader& head, headers& fields) noexcept
{
    while (true)
    {
        auto line = co_await head.next_line();
        if (line.has_error()) co_return line.to_error();
        if (line.to_value().empty()) co_return std::error_condition{};

        auto field = net::http::parse_field_line(line.to_value());
        if (!field.has_value()) co_return invalid();

        // NOTE: each line is kept whole: splitting on commas would break fields that aren't lists (e.g. User-Agent).
        fields.add(field->first, field->second);
    }
}

// count_values counts the field lines of key.
std::size_t count_values(const headers& fields, std::string_view key)
{
    auto all = fields.get_all(key);
    return all.has_value() ? static_cast<std::size_t>(std::ranges::distance(*all)) : 0;
}

// parse_host_header checks the Host header (RFC 9112, section 3.2), and sets the request's host.
std::error_condition parse_host_header(server_request& req)
{
    const auto lines = count_values(req.headers, "Host"sv);

    // HTTP/1.1 requires exactly one, and HTTP/1.0 doesn't know it, but mustn't repeat it either.
    if (lines > 1 || (lines == 0 && req.version.major == 1 && req.version.minor >= 1)) return invalid();

    std::string host;
    if (lines == 1)
    {
        auto value = req.headers.get("Host"sv).value_or(""sv);

        // an empty value is allowed, for targets without a host.
        if (!value.empty())
        {
            auto parsed = net::http::parse_host(value);
            if (!parsed.has_value()) return invalid();
            host = std::move(parsed->host);
        }
    }

    // a target with a host overrides the Host header (RFC 9112, section 3.2.2).
    req.host = !req.target.host.empty() ? req.target.host : std::move(host);
    return {};
}

// make_request_body decides how long the request's body is (RFC 9112, section 6.3), and wraps the reader to read only
// that much. The rules are strict where requests are ambiguous, as an ambiguity between this server and anything in
// front of it is what request smuggling exploits.
std::expected<std::unique_ptr<net::io::reader>, std::error_condition> make_request_body(buffered_reader*      reader,
                                                                                        const server_request& req)
{
    if (req.headers.get_all("Transfer-Encoding"sv).has_value())
    {
        // HTTP/1.0 has no transfer codings, and a length from both would be ambiguous.
        const bool http10 = req.version.major == 1 && req.version.minor == 0;
        if (http10 || req.headers.get_all("Content-Length"sv).has_value()) return std::unexpected(invalid());

        // chunked must be last, and only applied once (RFC 9112, section 6.1).
        auto codings  = req.headers.get_list("Transfer-Encoding"sv);
        auto chunked  = [](std::string_view coding) { return net::util::equal_ignore_case(coding, "chunked"sv); };
        auto how_many = std::ranges::count_if(codings, chunked);
        if (codings.empty() || !chunked(codings.back()) || how_many > 1) return std::unexpected(invalid());

        // and nothing else is implemented.
        if (codings.size() > 1) return std::unexpected(std::make_error_condition(std::errc::not_supported));

        return std::make_unique<net::http::http11::chunked_reader>(reader);
    }

    if (req.headers.get_all("Content-Length"sv).has_value())
    {
        auto length = req.headers.get_content_length();
        if (!length.has_value()) return std::unexpected(invalid());

        return std::make_unique<net::io::limit_reader>(static_cast<net::io::reader*>(reader), *length);
    }

    return std::make_unique<net::io::limit_reader>(static_cast<net::io::reader*>(reader), 0);
}

// response_framing is how a response's body ends (RFC 9112, section 6.3).
enum class response_framing : std::uint8_t
{
    none,      // there's no body.
    chunked,   // with the last chunk.
    length,    // after Content-Length bytes.
    connection // with the connection: if nothing else says, or after a 101 (when it's the new protocol's).
};

std::expected<response_framing, std::error_condition> framing_of(const client_response& resp, request_method method)
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
std::expected<std::unique_ptr<net::io::reader>, std::error_condition>
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
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access): only chosen when there's a valid Content-Length.
        return std::make_unique<net::io::limit_reader>(parent, *resp.headers.get_content_length());
    case response_framing::connection:
        return std::make_unique<net::io::limit_reader>(parent, std::numeric_limits<std::size_t>::max());
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

    [[nodiscard]] int native_handle() const noexcept override { return body->native_handle(); }

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
std::expected<request_framing, std::error_condition> frame_request(const client_request& req)
{
    using enum request_framing::kind;

    const auto version = effective_version(req);
    if (version.major != 1 || version.minor > 1) return std::unexpected(std::make_error_condition(std::errc::not_supported));

    const bool has_te = req.headers.get_all("Transfer-Encoding"sv).has_value();
    const bool has_cl = req.headers.get_all("Content-Length"sv).has_value();

    if (has_te)
    {
        // only chunked is implemented, and HTTP/1.0 has no transfer codings.
        auto codings = req.headers.get_list("Transfer-Encoding"sv);
        if (has_cl || version.minor == 0) return std::unexpected(invalid());
        if (codings.size() != 1 || !net::util::equal_ignore_case(codings.front(), "chunked"sv))
            return std::unexpected(std::make_error_condition(std::errc::not_supported));

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
    const bool expects_content = req.method == request_method::POST || req.method == request_method::PUT
                              || req.method == request_method::PATCH;
    return request_framing{.how = none, .length = 0, .field = expects_content ? "Content-Length: 0\r\n"sv : ""sv};
}

task<std::error_condition> write_full(net::io::writer& out, std::span<const std::byte> data)
{
    while (!data.empty())
    {
        auto res = co_await out.write(data);
        if (res.err) co_return res.err;
        if (res.count == 0) co_return make_error_condition(net::io::status_condition::error);

        data = data.subspan(res.count);
    }

    co_return std::error_condition{};
}

// body_buffer is how much of a body is read at a time.
constexpr std::size_t body_buffer = std::size_t{16} * 1'024;

// copy_length copies exactly length bytes from body, which mustn't end before then.
task<std::error_condition> copy_length(net::io::reader& body, net::io::writer& out, std::size_t length)
{
    std::vector<std::byte> buf(std::min(length, body_buffer));

    while (length > 0)
    {
        auto res = co_await body.read(std::span{buf}.first(std::min(buf.size(), length)));

        if (auto err = co_await write_full(out, std::span{buf}.first(res.count)); err) co_return err;
        length -= res.count;

        if (length == 0) break;

        // a body shorter than its Content-Length is the caller's mistake.
        if (res.err == net::io::status_condition::closed) co_return invalid();
        if (res.err) co_return res.err;
        if (res.count == 0) co_return make_error_condition(net::io::status_condition::error);
    }

    co_return std::error_condition{};
}

// copy_chunked copies body, until it ends, as chunks.
task<std::error_condition> copy_chunked(net::io::reader& body, net::io::writer& out)
{
    net::http::http11::chunked_writer chunked{&out};
    std::vector<std::byte>            buf(body_buffer);

    while (true)
    {
        auto res = co_await body.read(buf);

        if (res.count > 0)
        {
            if (auto written = co_await chunked.write(std::span{buf}.first(res.count)); written.err) co_return written.err;
        }

        if (res.err == net::io::status_condition::closed) break;
        if (res.err) co_return res.err;
        if (res.count == 0) co_return make_error_condition(net::io::status_condition::error);
    }

    co_return (co_await chunked.finish()).err;
}

// valid_fields checks that every field can be written without changing what's around it.
bool valid_fields(const headers& fields) noexcept
{
    return std::ranges::all_of(fields,
                               [](const auto& field)
                               {
                                   return net::http::is_valid_field_name(field.first)
                                       && std::ranges::all_of(field.second, net::http::is_valid_field_value);
                               });
}

bool is_default_port(std::string_view scheme, std::string_view port) noexcept
{
    return (port == "80"sv && (scheme.empty() || net::util::equal_ignore_case(scheme, "http"sv)))
        || (port == "443"sv && net::util::equal_ignore_case(scheme, "https"sv));
}

task<std::error_condition> write_headers(net::io::writer& writer, const headers& headers) noexcept
{
    // one line per value: joining them with commas would change fields that aren't lists (e.g. Set-Cookie).
    for (const auto& [name, values] : headers)
    {
        for (const auto& value : values)
        {
            const std::array<std::string_view, 4> parts{name, ": "sv, value, "\r\n"sv};
            for (auto part : parts)
            {
                auto res = co_await writer.write(part);
                if (res.err) co_return res.err;
            }
        }
    }

    co_return std::error_condition{};
}

}

namespace net::http::http11
{

using coro::task;

std::error_condition check_request(const client_request& req) noexcept
{
    try
    {
        if (!valid_fields(req.headers)) return invalid();

        auto framing = frame_request(req);
        if (!framing.has_value()) return framing.error();

        return {};
    }
    catch (const std::bad_alloc&)
    {
        return std::make_error_condition(std::errc::not_enough_memory);
    }
}

task<request_encoder_result> request_encode(io::writer* writer, const client_request& req) noexcept
{
    if (auto err = check_request(req); err) co_return std::unexpected(err);

    // NOTE: can't fail, having been checked.
    const auto framing = *frame_request(req);

    auto res = co_await writer->write(method_string(req.method));
    if (res.err) co_return std::unexpected(res.err);

    res = co_await writer->write(' ');
    if (res.err) co_return std::unexpected(res.err);

    // the target is written in origin-form ("/path?query"), encoded so that nothing in the url can end the line.
    res = co_await writer->write(req.uri.origin_form());
    if (res.err) co_return std::unexpected(res.err);

    res = co_await writer->write(" HTTP/"sv);
    if (res.err) co_return std::unexpected(res.err);

    // NOTE: unlike the status line, nothing follows the version.
    const auto          version = effective_version(req);
    std::array<char, 3> version_buf{
        static_cast<char>(version.major + '0'),
        '.',
        static_cast<char>(version.minor + '0'),
    };

    res = co_await writer->write(version_buf);
    if (res.err) co_return std::unexpected(res.err);

    res = co_await writer->write("\r\n"sv);
    if (res.err) co_return std::unexpected(res.err);

    // HTTP/1.1 requires a Host header.
    if (!req.headers.get("Host"sv).has_value() && !req.uri.host.empty())
    {
        // an IPv6 address is bracketed, so its colons aren't mistaken for the port's.
        auto host_line = req.uri.host.find(':') != std::string::npos ? "Host: [" + req.uri.host + "]" : "Host: " + req.uri.host;
        if (!req.uri.port.empty() && !is_default_port(req.uri.scheme, req.uri.port)) host_line += ":" + req.uri.port;
        host_line += "\r\n";

        res = co_await writer->write(host_line);
        if (res.err) co_return std::unexpected(res.err);
    }

    if (!framing.field.empty())
    {
        res = co_await writer->write(framing.field);
        if (res.err) co_return std::unexpected(res.err);
    }

    res.err = co_await write_headers(*writer, req.headers);
    if (res.err) co_return std::unexpected(res.err);

    res = co_await writer->write("\r\n"sv);
    if (res.err) co_return std::unexpected(res.err);

    try
    {
        std::error_condition err;

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
        co_return std::unexpected(std::make_error_condition(std::errc::not_enough_memory));
    }

    co_return writer;
}

task<response_encoder_result> response_encode(io::writer* writer, const server_response& resp) noexcept
{
    if (!valid_fields(resp.headers)) co_return std::unexpected(invalid());

    auto res = co_await writer->write("HTTP/"sv);
    if (res.err) co_return std::unexpected(res.err);

    std::array<char, 4> version_buf{
        static_cast<char>(resp.version.major + '0'),
        '.',
        static_cast<char>(resp.version.minor + '0'),
        ' ',
    };

    res = co_await writer->write(version_buf);
    if (res.err) co_return std::unexpected(res.err);

    res = co_await writer->write(status_text(resp.status_code));
    if (res.err) co_return std::unexpected(res.err);

    res = co_await writer->write("\r\n"sv);
    if (res.err) co_return std::unexpected(res.err);

    res.err = co_await write_headers(*writer, resp.headers);
    if (res.err) co_return std::unexpected(res.err);

    res = co_await writer->write("\r\n"sv);
    if (res.err) co_return std::unexpected(res.err);

    co_return writer;
}

task<request_decoder_result> request_decode(io::buffered_reader* reader, std::size_t max_header_bytes) noexcept
{
    try
    {
        server_request req;
        head_reader    head{.reader = reader, .remaining = max_header_bytes};

        auto line = co_await head.next_line();

        // an empty line before the request line is ignored (RFC 9112, section 2.2), e.g. a CRLF after a POST's body.
        if (line.has_value() && line.to_value().empty()) line = co_await head.next_line();
        if (line.has_error()) co_return std::unexpected(line.to_error());

        if (auto err = parse_request_line(line.to_value(), req); err) co_return std::unexpected(err);
        if (auto err = co_await parse_fields(head, req.headers); err) co_return std::unexpected(err);
        if (auto err = parse_host_header(req); err) co_return std::unexpected(err);

        auto body = make_request_body(reader, req);
        if (!body.has_value()) co_return std::unexpected(body.error());
        req.body = std::move(*body);

        co_return req;
    }
    catch (const std::bad_alloc&)
    {
        co_return std::unexpected(std::make_error_condition(std::errc::not_enough_memory));
    }
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

            auto line = co_await head.next_line();
            if (line.has_error()) co_return std::unexpected(line.to_error());

            if (auto err = parse_status_line(line.to_value(), resp); err) co_return std::unexpected(err);
            if (auto err = co_await parse_fields(head, resp.headers); err) co_return std::unexpected(err);
        } while (status_is_information(resp.status_code) && resp.status_code != status::SWITCHING_PROTOCOLS);

        auto body = make_response_body(reader, resp, method);
        if (!body.has_value()) co_return std::unexpected(body.error());
        resp.body = std::move(*body);

        co_return resp;
    }
    catch (const std::bad_alloc&)
    {
        co_return std::unexpected(std::make_error_condition(std::errc::not_enough_memory));
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
        co_return std::unexpected(std::make_error_condition(std::errc::not_enough_memory));
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
