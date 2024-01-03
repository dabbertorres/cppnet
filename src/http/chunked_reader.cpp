#include "http/chunked_reader.hpp"

#include <array>

namespace net::http::http11
{

io::result chunked_reader::read(std::byte* data, std::size_t length)
{
    // TODO: Return the ACTUAL number of bytes read (chunk lengths, "\r\n" sequences, etc),
    //       or just the bytes read INTO data?
    //       (Currently returning the number of bytes read into data.)

    std::size_t bytes_read = 0;

    while (bytes_read < length)
    {
        // new chunk
        if (current_chunk_size == 0)
        {
            auto res = get_next_chunk_size();
            if (res.err) return {.count = bytes_read, .err = res.err};

            // final chunk is size 0
            if (current_chunk_size == 0)
            {
                res = validate_end_of_chunk();
                if (res.err) return {.count = bytes_read, .err = res.err};

                break;
            }
        }

        // plain read of current chunk
        auto amount_to_read = std::min(length - bytes_read, current_chunk_size);
        auto res            = parent->read(data, amount_to_read);
        current_chunk_size -= res.count;
        bytes_read += res.count;
        if (res.err) return {.count = bytes_read, .err = res.err};

        // end of chunk
        if (current_chunk_size == 0)
        {
            res = validate_end_of_chunk();
            if (res.err) return {.count = bytes_read, .err = res.err};
        }
    }

    return {.count = bytes_read};
}

io::result chunked_reader::get_next_chunk_size()
{
    current_chunk_size = 0;

    while (true)
    {
        char next = 0;
        auto res  = parent->read(&next, 1);
        if (res.err) return res;

        if ('0' <= next && next <= '9')
        {
            current_chunk_size *= 10;
            current_chunk_size += static_cast<std::size_t>(next - '0');
        }
        else if (next == '\r')
        {
            // end of size - next byte should be a '\n'
            res = parent->read(&next, 1);
            if (res.err) return res;

            // done
            if (next == '\n') break;

            // nope, invalid
            return {.err = std::make_error_condition(std::errc::illegal_byte_sequence)};
        }
        else
        {
            return {.err = std::make_error_condition(std::errc::illegal_byte_sequence)};
        }
    }

    return {};
}

io::result chunked_reader::validate_end_of_chunk()
{
    std::array<char, 2> end_of_chunk{};

    auto res = parent->read(end_of_chunk.data(), end_of_chunk.size());
    if (res.err) return res;

    if (end_of_chunk[0] != '\r' && end_of_chunk[1] != '\n')
    {
        return {.err = std::make_error_condition(std::errc::illegal_byte_sequence)};
    }

    return {.count = 2};
}

}
