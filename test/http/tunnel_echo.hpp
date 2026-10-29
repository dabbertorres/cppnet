#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <system_error>

#include "coro/task.hpp"
#include "http/tunnel.hpp"
#include "io/buffered_reader.hpp"
#include "io/reader.hpp"

// For the tests of switching protocols (http::detail::switch_protocols()), over cleartext and TLS: a handler's side
// that echoes the tunnel's bytes, and a client's that reads what it's sent.
namespace test
{

// echo writes back what the tunnel reads, flushing it after each read (as it's read: so what was buffered before the
// switch first), until it has read "bye\n", returning why it stopped otherwise.
inline net::coro::task<std::error_code> echo(net::http::detail::tunnel& t)
{
    auto&       reader = t.reader();
    std::string seen;

    while (!seen.ends_with("bye\n"))
    {
        if (reader.size() == 0)
        {
            if (auto err = co_await reader.fill()) co_return err;
        }

        const auto data = reader.buffered();
        seen.append(reinterpret_cast<const char*>(data.data()), data.size());

        // NOTE: the writer copies what it buffers, so it's consumed after.
        auto wrote = co_await t.writer().write(data);
        reader.consume(data.size());
        if (wrote.err) co_return wrote.err;

        if (auto flushed = co_await t.writer().flush(); flushed.err) co_return flushed.err;
    }

    co_return std::error_code{};
}

// read_until reads conn into received until received contains what, returning the read's error if it can't.
inline net::coro::task<std::error_code> read_until(net::io::reader& conn, std::string& received, std::string_view what)
{
    std::array<std::byte, 4'096> buf{};
    while (!received.contains(what))
    {
        auto res = co_await conn.read(buf);
        received.append(reinterpret_cast<const char*>(buf.data()), res.count);
        if (res.err) co_return res.err;
    }

    co_return std::error_code{};
}

// read_to_end reads conn into received until it ends (or fails), returning why it stopped.
inline net::coro::task<std::error_code> read_to_end(net::io::reader& conn, std::string& received)
{
    std::array<std::byte, 4'096> buf{};
    while (true)
    {
        auto res = co_await conn.read(buf);
        received.append(reinterpret_cast<const char*>(buf.data()), res.count);
        if (res.err) co_return res.err;
    }
}

}
