#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "coro/sync_wait.hpp"
#include "coro/task.hpp"
#include "http/chunked_reader.hpp"
#include "http/chunked_writer.hpp"
#include "http/headers.hpp"
#include "http/http11.hpp"
#include "io/buffered_reader.hpp"
#include "io/buffered_writer.hpp"
#include "io/io.hpp"
#include "io/reader.hpp"
#include "io/string_reader.hpp"
#include "io/string_writer.hpp"
#include "io/writer.hpp"

namespace
{

using namespace std::string_view_literals;

// trickle_reader reads at most one byte at a time, as a slow connection might.
class trickle_reader : public net::io::reader
{
public:
    explicit trickle_reader(std::string_view text)
        : content{text}
    {}

    net::coro::task<net::io::result> read(std::span<std::byte> data) override
    {
        co_return co_await content.read(data.first(std::min<std::size_t>(data.size(), 1)));
    }

    using net::io::reader::read;

    [[nodiscard]] net::io::handle native_handle() const noexcept override { return -1; }

private:
    net::io::string_reader<char> content;
};

// trickle_writer writes at most one byte at a time.
class trickle_writer : public net::io::writer
{
public:
    net::coro::task<net::io::result> write(std::span<const std::byte> data) override
    {
        co_return co_await out.write(data.first(std::min<std::size_t>(data.size(), 1)));
    }

    using net::io::writer::write;

    [[nodiscard]] net::io::handle native_handle() const noexcept override { return -1; }

    std::string build() { return out.build(); }

private:
    net::io::string_writer<char> out;
};

struct read_result
{
    std::string     body;
    std::error_code err; // other than the end of the body
};

// read_chunked reads a chunked body from text, and returns it, and what follows it, keeping its trailers in trailers
// (if it isn't null). By default, the text arrives a byte at a time.
std::pair<read_result, std::string>
read_chunked(std::string_view text, bool trickle = true, net::http::trailer_fields* trailers = nullptr)
{
    trickle_reader                    trickled{text};
    net::io::string_reader<char>      whole{text};
    net::io::buffered_reader          buffered{trickle ? static_cast<net::io::reader*>(&trickled) : &whole};
    net::http::http11::chunked_reader chunked{&buffered};
    chunked.keep_trailers(trailers);

    read_result out;
    std::string buf(7, '\0');

    while (true)
    {
        auto res = test::sync_wait(chunked.read(buf));
        out.body.append(buf.data(), res.count);
        if (res.err)
        {
            if (res.err != net::io::errc::end_of_stream) out.err = res.err;
            break;
        }
    }

    std::string rest;
    while (true)
    {
        auto res = test::sync_wait(buffered.read(buf));
        rest.append(buf.data(), res.count);
        if (res.err) break;
    }

    return {out, rest};
}

// lines lists fields' field lines, in order.
std::vector<std::string> lines(net::http::fields_view fields)
{
    std::vector<std::string> out;
    for (const auto& [name, values] : fields)
    {
        for (const auto& value : values) out.push_back(std::string{name}.append(": ").append(value));
    }

    return out;
}

// many_trailers makes a body whose trailer section has count field lines of field.
std::string many_trailers(std::size_t count, std::string_view field)
{
    std::string text = "5\r\nhello\r\n0\r\n";
    for (std::size_t i = 0; i < count; ++i) text.append(field).append("\r\n");
    return text + "\r\n";
}

}

TEST_CASE("chunked bodies are decoded", "[http][1.1][chunked][chunked_reader]")
{
    struct test_case
    {
        std::string_view name;
        std::string_view input;
        std::string_view expect;
    };

    auto tc = GENERATE(
        test_case{.name   = "hex sizes",
                  .input  = "a\r\n0123456789\r\n1A\r\nabcdefghijklmnopqrstuvwxyz\r\n0\r\n\r\nNEXT",
                  .expect = "0123456789abcdefghijklmnopqrstuvwxyz"},
        test_case{.name = "leading zeros", .input = "005\r\nhello\r\n000\r\n\r\nNEXT", .expect = "hello"},
        test_case{.name = "extensions", .input = "5;name=value;flag\r\nhello\r\n0;last\r\n\r\nNEXT", .expect = "hello"},
        test_case{.name   = "trailers",
                  .input  = "5\r\nhello\r\n0\r\nX-Checksum: abc\r\nX-Other: def\r\n\r\nNEXT",
                  .expect = "hello"},
        test_case{.name = "empty", .input = "0\r\n\r\nNEXT", .expect = ""});

    CAPTURE(tc.name);

    auto [result, rest] = read_chunked(tc.input);
    CHECK_FALSE(result.err);
    CHECK(result.body == tc.expect);

    // nothing after the body is read.
    CHECK(rest == "NEXT");
}

TEST_CASE("malformed chunked bodies are rejected", "[http][1.1][chunked][chunked_reader]")
{
    auto input = GENERATE("5\r\nhelloXX0\r\n\r\n"sv,
                          "5\r\nhello\rX0\r\n\r\n"sv,
                          "5\r\nhelloX\n0\r\n\r\n"sv,
                          "\r\nhello\r\n0\r\n\r\n"sv,
                          "g\r\nhello\r\n0\r\n\r\n"sv,
                          "5 \r\nhello\r\n0\r\n\r\n"sv,
                          "5\nhello\r\n0\r\n\r\n"sv,
                          "-5\r\nhello\r\n0\r\n\r\n"sv,
                          "10000000000000000\r\nhello\r\n0\r\n\r\n"sv,
                          "5\r\nhello\r\n0\r\nX-Bad trailer\r\n\r\n"sv,
                          "5\r\nhel"sv);
    CAPTURE(input);

    auto [result, rest] = read_chunked(input);
    CHECK(result.err);
}

TEST_CASE("chunk size lines are limited in size", "[http][1.1][chunked][chunked_reader]")
{
    // (not a byte at a time: that would take a co_await per byte, which GCC doesn't keep off the stack in debug
    // builds.)
    auto [result, rest] = read_chunked("5;" + std::string(100'000, 'x') + "\r\nhello\r\n0\r\n\r\n", false);
    CHECK(result.err == std::errc::value_too_large);
}

TEST_CASE("chunked bodies are encoded", "[http][1.1][chunked][chunked_writer]")
{
    trickle_writer                    out;
    net::http::http11::chunked_writer chunked{&out};

    auto res = test::sync_wait(chunked.write("hello"sv));
    CHECK_FALSE(res.err);
    CHECK(res.count == 5);

    // an empty write mustn't write an empty chunk, which would end the body.
    res = test::sync_wait(chunked.write(""sv));
    CHECK_FALSE(res.err);

    const std::string big(300, 'x');
    res = test::sync_wait(chunked.write(big));
    CHECK_FALSE(res.err);
    CHECK(res.count == big.size());

    res = test::sync_wait(chunked.finish());
    CHECK_FALSE(res.err);

    CHECK(out.build() == "5\r\nhello\r\n12c\r\n" + big + "\r\n0\r\n\r\n");

    // and it reads back.
    auto [result, rest] = read_chunked(out.build());
    CHECK_FALSE(result.err);
    CHECK(result.body == "hello" + big);
}

TEST_CASE("chunked bodies keep their trailers", "[http][1.1][chunked][chunked_reader][trailers]")
{
    const auto trickle = GENERATE(true, false);
    CAPTURE(trickle);

    net::http::trailer_fields trailers;
    trailers.set("X-Stale", "from before"); // (cleared when the trailer section is read)

    auto [result, rest] = read_chunked("5\r\nhello\r\n0\r\n"
                                       "X-Checksum: abc\r\n"
                                       "Server-Timing:  db;dur=53 \r\n"
                                       "If-Match: \"xyz\"\r\n"
                                       "x-checksum: def\r\n"
                                       "Content-Length: 99\r\n"
                                       "grpc-status: 0\r\n"
                                       "X-Empty:\r\n"
                                       "\r\nNEXT",
                                       trickle,
                                       &trailers);
    CHECK_FALSE(result.err);
    CHECK(result.body == "hello");
    CHECK(rest == "NEXT");

    // repeated names are kept as one field with each value, in order (as headers keeps them), and those that can't be
    // trailers are dropped.
    CHECK(lines(trailers)
          == std::vector<std::string>{"X-Checksum: abc",
                                      "X-Checksum: def",
                                      "Server-Timing: db;dur=53",
                                      "grpc-status: 0",
                                      "X-Empty: "});
}

TEST_CASE("chunked bodies without trailers leave none", "[http][1.1][chunked][chunked_reader][trailers]")
{
    net::http::trailer_fields trailers;
    trailers.set("X-Stale", "from before");

    auto [result, rest] = read_chunked("5\r\nhello\r\n0\r\n\r\n", true, &trailers);
    CHECK_FALSE(result.err);
    CHECK(trailers.empty());
}

TEST_CASE("trailer sections are limited", "[http][1.1][chunked][chunked_reader][trailers]")
{
    // whether they're kept or not, so a body decodes the same either way.
    const auto keep = GENERATE(true, false);
    CAPTURE(keep);

    net::http::trailer_fields trailers;
    auto*                     into = keep ? &trailers : nullptr;

    SECTION("up to max_fields field lines")
    {
        auto [result, rest] = read_chunked(many_trailers(net::http::http11::max_fields, "X-A: b"), false, into);
        CHECK_FALSE(result.err);
        if (keep) CHECK(lines(trailers).size() == net::http::http11::max_fields);
    }

    SECTION("but no more")
    {
        auto [result, rest] = read_chunked(many_trailers(net::http::http11::max_fields + 1, "X-A: b"), false, into);
        CHECK(result.err == std::errc::value_too_large);
        CHECK(trailers.empty());
    }

    SECTION("dropped fields count too")
    {
        auto [result, rest] = read_chunked(many_trailers(net::http::http11::max_fields + 1, "If-A: b"), false, into);
        CHECK(result.err == std::errc::value_too_large);
    }

    SECTION("and the section is at most max_trailers bytes")
    {
        // a few long lines, so it's the size that's over.
        const std::string field = "X-Long: " + std::string(1'000, 'x');
        auto [result, rest]     = read_chunked(many_trailers(9, field), false, into);
        CHECK(result.err == std::errc::value_too_large);
        CHECK(trailers.empty());

        std::tie(result, rest) = read_chunked(many_trailers(8, field), false, into);
        CHECK_FALSE(result.err);
    }
}

TEST_CASE("malformed trailers fail the body", "[http][1.1][chunked][chunked_reader][trailers]")
{
    const auto keep = GENERATE(true, false);
    auto       line = GENERATE("X-Bad trailer"sv,
                               "X-Bad : space before the colon"sv,
                               " X-Folded: value"sv,
                               ": no name"sv,
                               "X(Bad): not a token"sv,
                               "X-Control: a\x01"sv,
                               "X-Delete: a\x7f"sv);
    CAPTURE(keep, line);

    net::http::trailer_fields trailers;
    const auto text = "5\r\nhello\r\n0\r\nX-Good: kept\r\n" + std::string{line} + "\r\nX-After: too\r\n\r\n";

    auto [result, rest] = read_chunked(text, true, keep ? &trailers : nullptr);
    CHECK(result.err == std::errc::illegal_byte_sequence);

    // and none of the section is kept.
    CHECK(trailers.empty());
}

TEST_CASE("trailers are kept before the end callback is called", "[http][1.1][chunked][chunked_reader][trailers]")
{
    struct seen
    {
        net::http::trailer_fields* trailers = nullptr;
        int                        calls    = 0;
        std::string                checksum;
    };

    net::http::trailer_fields trailers;
    seen                      state{.trailers = &trailers, .calls = 0, .checksum = {}};

    net::io::string_reader<char>      in{"3\r\nabc\r\n0\r\nX-Checksum: 900150983cd24fb0\r\n\r\n"sv};
    net::io::buffered_reader          buffered{&in};
    net::http::http11::chunked_reader chunked{&buffered};
    chunked.keep_trailers(&trailers);
    chunked.call_at_end({.call =
                             [](void* context) noexcept
                         {
                             auto* state = static_cast<seen*>(context);
                             ++state->calls;
                             state->checksum = state->trailers->get("X-Checksum").value_or("");
                         },
                         .context = &state});

    std::string buf(16, '\0');
    auto        res = test::sync_wait(chunked.read(buf));
    CHECK(res.count == 3);
    CHECK_FALSE(res.err);
    CHECK(state.calls == 0);

    res = test::sync_wait(chunked.read(buf));
    CHECK(res.err == net::io::errc::end_of_stream);
    CHECK(state.calls == 1);
    CHECK(state.checksum == "900150983cd24fb0");
}

TEST_CASE("chunked bodies are encoded with trailers", "[http][1.1][chunked][chunked_writer][trailers]")
{
    // through a buffered writer with room for the last chunk (formatted into its buffer, without a coroutine), one
    // without (written whole), and a writer that's not buffered.
    enum class kind : std::uint8_t
    {
        buffered,
        small_buffer,
        trickle,
    };

    const auto how = GENERATE(kind::buffered, kind::small_buffer, kind::trickle);
    CAPTURE(how);

    trickle_writer           trickled;
    net::io::buffered_writer buffered{&trickled, how == kind::small_buffer ? 16U : 1'024U};
    net::io::writer*         out = how == kind::trickle ? static_cast<net::io::writer*>(&trickled) : &buffered;

    net::http::http11::chunked_writer chunked{out};

    // what's written, once flushed.
    auto written = [&]
    {
        if (how != kind::trickle) CHECK_FALSE(test::sync_wait(buffered.flush()).err);
        return trickled.build();
    };

    auto res = test::sync_wait(chunked.write("hello"sv));
    REQUIRE_FALSE(res.err);

    SECTION("several, with a repeated name")
    {
        net::http::trailer_fields trailers;
        trailers.set("X-Checksum", "abc");
        trailers.set("Server-Timing", {"db;dur=53", "app;dur=47.2"});
        trailers.set("grpc-status", "0");

        res = test::sync_wait(chunked.finish(&trailers));
        CHECK_FALSE(res.err);
        CHECK(
            written()
            == "5\r\nhello\r\n0\r\n"
               "X-Checksum: abc\r\nServer-Timing: db;dur=53\r\nServer-Timing: app;dur=47.2\r\ngrpc-status: 0\r\n\r\n");
    }

    SECTION("none")
    {
        const net::http::trailer_fields empty;
        const bool                      given = GENERATE(true, false);
        res                                   = test::sync_wait(given ? chunked.finish(&empty) : chunked.finish());
        CHECK_FALSE(res.err);
        CHECK(written() == "5\r\nhello\r\n0\r\n\r\n");
    }

    SECTION("those that can't be trailers are dropped")
    {
        net::http::trailer_fields trailers;
        trailers.set("Content-Length", "5");
        trailers.set("X-Kept", "yes");
        trailers.set("If-None-Match", "*");
        trailers.set("trailer", "X-Kept");

        res = test::sync_wait(chunked.finish(&trailers));
        CHECK_FALSE(res.err);
        CHECK(written() == "5\r\nhello\r\n0\r\nX-Kept: yes\r\n\r\n");
    }

    SECTION("an invalid one writes nothing")
    {
        net::http::trailer_fields trailers;
        trailers.set("X-Fine", "yes");
        const auto name  = GENERATE("X-Bad"sv, "Bad Name"sv, ""sv);
        const auto value = GENERATE("a\r\nX-Injected: b"sv, "fine"sv);
        CAPTURE(name, value);

        trailers.set(name, value);
        if (valid_fields(trailers)) trailers.set("X-Other", "a\nb");

        res = test::sync_wait(chunked.finish(&trailers));
        CHECK(res.err == std::errc::invalid_argument);

        // so the caller can cut the body short: nothing of the last chunk is written.
        CHECK(written() == "5\r\nhello\r\n");
    }
}

TEST_CASE("trailers read back as they were written", "[http][1.1][chunked][trailers]")
{
    net::http::trailer_fields sent;
    sent.set("X-Checksum", "abc");
    sent.set("Server-Timing", {"db;dur=53", "app;dur=47.2"});
    sent.set("grpc-message", "a message, with spaces\tand a tab");
    sent.set("Content-Type", "text/plain"); // (dropped, by both)

    trickle_writer                    out;
    net::http::http11::chunked_writer chunked{&out};
    CHECK_FALSE(test::sync_wait(chunked.write("hello"sv)).err);
    CHECK_FALSE(test::sync_wait(chunked.finish(&sent)).err);

    net::http::trailer_fields received;
    auto [result, rest] = read_chunked(out.build(), true, &received);
    CHECK_FALSE(result.err);
    CHECK(result.body == "hello");

    sent.remove("Content-Type");
    CHECK(lines(received) == lines(sent));
}
