#include "io/util.hpp"

#include <cstddef>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "coro/task.hpp"
#include "io/buffered_reader.hpp"
#include "io/io.hpp"

namespace net::io
{

namespace
{

// read_line reads up to end_of_line, from what's buffered: it only co_awaits to refill the buffer, rather than for
// each byte (see buffered_reader::buffered()).
coro::task<readline_result>
read_line(buffered_reader* reader, std::size_t max_length, std::string_view end_of_line, bool end_of_stream_ends_line)
{
    std::string line;

    while (!line.ends_with(end_of_line))
    {
        auto available = reader->buffered();
        if (available.empty())
        {
            auto [_, have_next] = co_await reader->peek();
            if (have_next) continue;

            // the end of the stream may end the line, but anything else (e.g. a timeout) is an error.
            auto err = reader->error();
            if (err && err != status_condition::closed) co_return err;
            if (!end_of_stream_ends_line) co_return make_error_condition(status_condition::closed);
            break;
        }

        std::size_t taken = 0;
        while (taken < available.size() && !line.ends_with(end_of_line))
        {
            if (line.size() == max_length)
            {
                reader->consume(taken);
                co_return std::make_error_condition(std::errc::value_too_large);
            }

            line += static_cast<char>(available[taken++]);
        }

        reader->consume(taken);
    }

    if (line.ends_with(end_of_line)) line.resize(line.size() - end_of_line.size());
    co_return readline_result{std::move(line)};
}

}

coro::task<readline_result> readline(buffered_reader* reader, std::string_view end_of_line) noexcept
{
    co_return co_await read_line(reader, std::numeric_limits<std::size_t>::max(), end_of_line, true);
}

coro::task<readline_result>
read_terminated_line(buffered_reader* reader, std::size_t max_length, std::string_view end_of_line) noexcept
{
    co_return co_await read_line(reader, max_length, end_of_line, false);
}

}
