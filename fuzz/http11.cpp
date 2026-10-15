// fuzz_http11 decodes a connection's worth of requests (and their bodies), and checks that:
// - decoding doesn't crash (the decoders are noexcept, so exceptions count).
// - it decodes exactly the same when the connection delivers one byte at a time, so nothing depends on how reads are
//   split up (e.g. a short read in the middle of a chunk's CRLF).
// - every field it accepts can be written back out unchanged: no line breaks or other controls get through.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string>
#include <string_view>

#include "coro/sync_wait.hpp"
#include "coro/task.hpp"
#include "http/headers.hpp"
#include "http/http11.hpp"
#include "http/request.hpp"
#include "io/buffered_reader.hpp"
#include "io/io.hpp"
#include "io/reader.hpp"
#include "io/string_reader.hpp"

namespace
{

// trickle_reader reads at most one byte at a time.
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

[[noreturn]] void fail(const char* what, std::string_view a, std::string_view b = {})
{
    std::fprintf(stderr, "PROPERTY FAILED: %s\n  [%.*s]\n  [%.*s]\n", what, int(a.size()), a.data(), int(b.size()),
                 b.data());
    std::abort();
}

// transcript decodes requests from reader as the server would, and describes what it decoded.
std::string transcript(net::io::reader* conn)
{
    net::io::buffered_reader reader{conn};
    std::string              out;

    for (int i = 0; i < 8; ++i)
    {
        auto req = test::sync_wait(net::http::http11::request_decode(&reader, 512));
        if (!req.has_value())
        {
            out += "error: " + req.error().message() + "\n";
            return out;
        }

        out += std::string{net::http::method_string(req->method)} + " " + req->target.path + "?" + req->target.query
             + " " + std::to_string(req->version.major) + "." + std::to_string(req->version.minor) + " host=" + req->host
             + "\n";

        for (const auto& [name, values] : req->headers)
        {
            if (!net::http::is_valid_field_name(name)) fail("accepted an invalid field name", name);

            for (const auto& value : values)
            {
                if (!net::http::is_valid_field_value(value)) fail("accepted an invalid field value", value);
                out += name + ": " + value + "\n";
            }
        }

        // reads of an odd size, so they straddle chunks.
        std::string body;
        std::string buf(13, '\0');
        while (true)
        {
            auto res = test::sync_wait(req->body->read(buf));
            body.append(buf.data(), res.count);

            if (res.err)
            {
                out += "body: " + body + " (" + res.err.message() + ")\n";
                if (res.err != net::io::status_condition::closed) return out;
                break;
            }
        }
    }

    return out;
}

}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    std::string_view input{reinterpret_cast<const char*>(data), size};

    net::io::string_reader<char> whole{input};
    trickle_reader               trickle{input};

    auto expect = transcript(&whole);
    auto got    = transcript(&trickle);
    if (expect != got) fail("reading a byte at a time decoded differently", expect, got);

    return 0;
}
