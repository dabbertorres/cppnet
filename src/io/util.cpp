#include "io/util.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

#include "coro/task.hpp"
#include "io/buffered_reader.hpp"
#include "io/io.hpp"

namespace net::io
{

readline_result readline(buffered_reader* reader, std::string_view end_of_line) noexcept
{
    // TODO: max read

    std::string line;

    const auto add_size = [&]
    {
        if (line.size() == line.capacity())
        {
            auto new_size = static_cast<double>(line.size()) * 1.5;
            line.reserve(static_cast<std::size_t>(new_size));
        }

        line.resize(line.size() + 1);
    };

    while (!line.ends_with(end_of_line))
    {
        auto [next, have_next] = reader->peek();
        if (!have_next) break; // nothing more to read

        add_size();
        auto [count, err] = reader->read(std::span{line.data() + (line.size() - 1), 1});
        if (err) return err;
        if (count == 0) return {make_error_condition(status_condition::closed)}; // TODO: communicate the actual error
    }

    if (line.ends_with(end_of_line)) line.resize(line.size() - end_of_line.size());

    return {line};
}

coro::task<readline_result> co_readline(buffered_reader* reader, std::string_view end_of_line) noexcept
{
    // TODO: max read

    std::string line;

    const auto add_size = [&]
    {
        if (line.size() == line.capacity())
        {
            auto new_size = static_cast<double>(line.size()) * 1.5;
            line.reserve(static_cast<std::size_t>(new_size));
        }

        line.resize(line.size() + 1);
    };

    while (!line.ends_with(end_of_line))
    {
        auto [next, have_next] = reader->peek();
        if (!have_next) break; // nothing more to read

        add_size();
        auto [count, err] = co_await reader->co_read(std::span{line.data() + (line.size() - 1), 1});
        if (err) co_return err;
        if (count == 0)
            co_return readline_result{
                make_error_condition(status_condition::closed)}; // TODO: communicate the actual error
    }

    if (line.ends_with(end_of_line)) line.resize(line.size() - end_of_line.size());

    co_return readline_result{line};
}

}
