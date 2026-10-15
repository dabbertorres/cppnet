#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "coro/sync_wait.hpp"
#include "coro/task.hpp"
#include "http/chunked_reader.hpp"
#include "http/chunked_writer.hpp"
#include "io/buffered_reader.hpp"
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

    [[nodiscard]] int native_handle() const noexcept override { return -1; }

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

    [[nodiscard]] int native_handle() const noexcept override { return -1; }

    std::string build() { return out.build(); }

private:
    net::io::string_writer<char> out;
};

struct read_result
{
    std::string          body;
    std::error_condition err; // other than the end of the body
};

// read_chunked reads a chunked body from text, and returns it, and what follows it. By default, the text arrives a byte
// at a time.
std::pair<read_result, std::string> read_chunked(std::string_view text, bool trickle = true)
{
    trickle_reader               trickled{text};
    net::io::string_reader<char> whole{text};
    net::io::buffered_reader     buffered{trickle ? static_cast<net::io::reader*>(&trickled) : &whole};
    net::http::http11::chunked_reader    chunked{&buffered};

    read_result out;
    std::string buf(7, '\0');

    while (true)
    {
        auto res = test::sync_wait(chunked.read(buf));
        out.body.append(buf.data(), res.count);
        if (res.err)
        {
            if (res.err != net::io::status_condition::closed) out.err = res.err;
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
        test_case{.name = "hex sizes", .input = "a\r\n0123456789\r\n1A\r\nabcdefghijklmnopqrstuvwxyz\r\n0\r\n\r\nNEXT", .expect = "0123456789abcdefghijklmnopqrstuvwxyz"},
        test_case{.name = "leading zeros", .input = "005\r\nhello\r\n000\r\n\r\nNEXT", .expect = "hello"},
        test_case{.name = "extensions", .input = "5;name=value;flag\r\nhello\r\n0;last\r\n\r\nNEXT", .expect = "hello"},
        test_case{.name = "trailers", .input = "5\r\nhello\r\n0\r\nX-Checksum: abc\r\nX-Other: def\r\n\r\nNEXT", .expect = "hello"},
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
    // (not a byte at a time: that would take a co_await per byte, which GCC doesn't keep off the stack in debug builds.)
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
