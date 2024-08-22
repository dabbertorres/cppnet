#include "http/chunked_reader.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <system_error>

#include "coro/task.hpp"
#include "io/io.hpp"

namespace net::http::http11
{

coro::task<io::result> chunked_reader::read(std::span<std::byte> data)
{
    // TODO: Return the ACTUAL number of bytes read (chunk lengths, "\r\n" sequences, etc),
    //       or just the bytes read INTO data?
    //       (Currently returning the number of bytes read into data.)

    std::size_t bytes_read = 0;

    while (bytes_read < data.size())
    {
        // new chunk
        if (current_chunk_size == 0)
        {
            auto res = co_await get_next_chunk_size();
            if (res.err) co_return {.count = bytes_read, .err = res.err};

            // final chunk is size 0
            if (current_chunk_size == 0)
            {
                res = co_await validate_end_of_chunk();
                if (res.err) co_return {.count = bytes_read, .err = res.err};

                break;
            }
        }

        // plain read of current chunk
        auto amount_to_read = std::min(data.size() - bytes_read, current_chunk_size);
        auto res            = co_await parent->read(data.subspan(bytes_read, amount_to_read));
        current_chunk_size -= res.count;
        bytes_read += res.count;
        if (res.err) co_return {.count = bytes_read, .err = res.err};

        // end of chunk
        if (current_chunk_size == 0)
        {
            res = co_await validate_end_of_chunk();
            if (res.err) co_return {.count = bytes_read, .err = res.err};
        }
    }

    co_return {.count = bytes_read};
}

coro::task<io::result> chunked_reader::get_next_chunk_size()
{
    current_chunk_size = 0;

    while (true)
    {
        char next = 0;
        auto res  = co_await parent->read(next);
        if (res.err) co_return res;

        if ('0' <= next && next <= '9')
        {
            current_chunk_size *= 10;
            current_chunk_size += static_cast<std::size_t>(next - '0');
        }
        else if (next == '\r')
        {
            // end of size - next byte should be a '\n'
            res = co_await parent->read(next);
            if (res.err) co_return res;

            // done
            if (next == '\n') break;

            // nope, invalid
            co_return {.err = std::make_error_condition(std::errc::illegal_byte_sequence)};
        }
        else
        {
            co_return {.err = std::make_error_condition(std::errc::illegal_byte_sequence)};
        }
    }

    co_return {};
}

coro::task<io::result> chunked_reader::validate_end_of_chunk()
{
    std::array<char, 2> end_of_chunk{};

    auto res = co_await parent->read(std::span{end_of_chunk});
    if (res.err) co_return res;

    if (end_of_chunk[0] != '\r' && end_of_chunk[1] != '\n')
    {
        co_return {.err = std::make_error_condition(std::errc::illegal_byte_sequence)};
    }

    co_return {.count = 2};
}

}
