#pragma once

#include <chrono>
#include <system_error>

#include "coro/generator.hpp"
#include "coro/task.hpp"
#include "io/io.hpp"
#include "io/reader.hpp"
#include "io/writer.hpp"

#include "listen.hpp"

namespace net::io::aio
{

class event_loop
{
public:
    using operation = coro::task<io::result>;

    event_loop();

    event_loop(event_loop&&) noexcept            = default;
    event_loop& operator=(event_loop&&) noexcept = default;

    event_loop(const event_loop&)            = delete;
    event_loop& operator=(const event_loop&) = delete;

    ~event_loop();

    coro::generator<tcp_socket> accept(const listener* listener);

    operation write(io::writer* writer, const std::byte* data, std::size_t length);
    operation write(io::writer* writer, const char* data, std::size_t length);
    operation write(io::writer* writer, std::string_view data);
    operation write(io::writer* writer, const std::byte* data, std::size_t length, std::chrono::microseconds timeout);
    operation write(io::writer* writer, const char* data, std::size_t length, std::chrono::microseconds timeout);
    operation write(io::writer* writer, std::string_view data, std::chrono::microseconds timeout);

    operation read(io::reader* reader, std::byte* data, std::size_t length);
    operation read(io::reader* reader, char* data, std::size_t length);
    operation read(io::reader* reader, std::byte* data, std::size_t length, std::chrono::microseconds timeout);
    operation read(io::reader* reader, char* data, std::size_t length, std::chrono::microseconds timeout);

private:
    operation write(io::writer* writer, const std::byte* data, std::size_t length, struct timespec* timeout);
    operation read(io::reader* reader, std::byte* data, std::size_t length, struct timespec* timeout);

    // TODO: OS/system specifics
    int queue;
};

}
