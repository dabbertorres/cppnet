#include "io/limit_reader.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <exception> // IWYU pragma: keep
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "coro/sync_wait.hpp"
#include "coro/task.hpp"
#include "io/io.hpp"
#include "io/reader.hpp"

using namespace std::string_view_literals;

namespace
{

// step is what one read of a scripted_reader returns: data, and an error.
struct step
{
    std::string_view data;
    std::error_code  err;
};

// scripted_reader returns its steps in turn (then the end of the stream), each from a read that finishes without a
// coroutine if ready, or from a coroutine otherwise, so both of limit_reader's ways are tested.
class scripted_reader final : public net::io::reader
{
public:
    scripted_reader(std::vector<step> steps, bool ready)
        : steps{std::move(steps)}
        , ready{ready}
    {}

    net::coro::task<net::io::result> read(std::span<std::byte> data) override
    {
        auto res = next(data);
        if (ready) return net::coro::ready_task<net::io::result>(res);
        return later(res);
    }

    using net::io::reader::read;

    [[nodiscard]] net::io::handle native_handle() const noexcept override { return 0; }

    // asked is the most each read asked for, in turn.
    std::vector<std::size_t> asked;

private:
    static net::coro::task<net::io::result> later(net::io::result res) { co_return res; }

    net::io::result next(std::span<std::byte> data)
    {
        asked.push_back(data.size());
        if (at == steps.size()) return {.count = 0, .err = make_error_code(net::io::errc::end_of_stream)};

        auto& [rest, err] = steps[at];
        const auto count  = std::min(rest.size(), data.size());
        std::ranges::copy(std::as_bytes(std::span{rest}.first(count)), data.begin());
        rest.remove_prefix(count);

        // the step's error comes with the last of its data.
        if (!rest.empty()) return {.count = count};
        ++at;
        return {.count = count, .err = err};
    }

    std::vector<step> steps;
    std::size_t       at = 0;
    bool              ready;
};

// outcome is all that reading a reader to its end read, and how it ended.
struct outcome
{
    std::string     data;
    std::error_code err;
};

outcome read_to_end(net::io::reader& reader)
{
    outcome                  out;
    std::array<std::byte, 4> buf{};
    while (true)
    {
        auto res = test::sync_wait(reader.read(buf));
        out.data.append(reinterpret_cast<const char*>(buf.data()), res.count);
        if (res.err || res.count == 0)
        {
            out.err = res.err;
            return out;
        }
    }
}

const auto end_of_stream = make_error_code(net::io::errc::end_of_stream);

}

TEST_CASE("a limit_reader reads up to its limit, and then ends", "[io][limit_reader]")
{
    const bool ready = GENERATE(true, false);
    CAPTURE(ready);

    SECTION("with more after the limit, which isn't read")
    {
        scripted_reader       parent{{{.data = "abcdefghij"sv, .err = end_of_stream}}, ready};
        net::io::limit_reader limited{&parent, 6};

        auto out = read_to_end(limited);
        CHECK(out.data == "abcdef");
        CHECK(out.err == net::io::errc::end_of_stream);

        // two reads of 4 bytes would be 8: the second only asks for what's left, and nothing's read after it.
        CHECK(parent.asked == std::vector<std::size_t>{4, 2});
    }

    SECTION("ending exactly at the limit")
    {
        scripted_reader       parent{{{.data = "abcdef"sv, .err = end_of_stream}}, ready};
        net::io::limit_reader limited{&parent, 6};

        auto out = read_to_end(limited);
        CHECK(out.data == "abcdef");
        CHECK(out.err == net::io::errc::end_of_stream);
    }

    SECTION("ending at the limit, then reporting the end on the next read")
    {
        scripted_reader       parent{{{.data = "abcdef"sv, .err = {}}}, ready};
        net::io::limit_reader limited{&parent, 6};

        auto out = read_to_end(limited);
        CHECK(out.data == "abcdef");
        CHECK(out.err == net::io::errc::end_of_stream);
        CHECK(parent.asked == std::vector<std::size_t>{4, 2});
    }

    SECTION("a limit of 0 reads nothing")
    {
        scripted_reader       parent{{{.data = "abc"sv, .err = {}}}, ready};
        net::io::limit_reader limited{&parent, 0};

        auto out = read_to_end(limited);
        CHECK(out.data.empty());
        CHECK(out.err == net::io::errc::end_of_stream);
        CHECK(parent.asked.empty());
    }
}

TEST_CASE("a limit_reader whose stream ends before its limit fails, unless the limit is only a cap",
          "[io][limit_reader]")
{
    const bool ready = GENERATE(true, false);
    CAPTURE(ready);

    SECTION("the end comes on a read of its own")
    {
        scripted_reader       parent{{{.data = "abc"sv, .err = {}}}, ready};
        net::io::limit_reader limited{&parent, 6};

        auto out = read_to_end(limited);
        CHECK(out.data == "abc");
        CHECK(out.err == std::errc::connection_aborted);
    }

    SECTION("the end comes with the last of the data, which is still read")
    {
        scripted_reader       parent{{{.data = "abc"sv, .err = end_of_stream}}, ready};
        net::io::limit_reader limited{&parent, 6};

        std::array<std::byte, 8> buf{};
        auto                     res = test::sync_wait(limited.read(buf));
        CHECK(res.count == 3);
        CHECK(res.err == std::errc::connection_aborted);

        // and again, as the stream's still short.
        res = test::sync_wait(limited.read(buf));
        CHECK(res.count == 0);
        CHECK(res.err == std::errc::connection_aborted);
    }

    SECTION("other failures are reported as they are")
    {
        scripted_reader       parent{{{.data = "abc"sv, .err = std::make_error_code(std::errc::timed_out)}}, ready};
        net::io::limit_reader limited{&parent, 6};

        auto out = read_to_end(limited);
        CHECK(out.data == "abc");
        CHECK(out.err == std::make_error_code(std::errc::timed_out));
    }

    SECTION("up_to: the stream may end before the limit")
    {
        scripted_reader       parent{{{.data = "abc"sv, .err = {}}}, ready};
        net::io::limit_reader limited{&parent, 6, net::io::limit_reader::mode::up_to};

        auto out = read_to_end(limited);
        CHECK(out.data == "abc");
        CHECK(out.err == net::io::errc::end_of_stream);
    }
}
