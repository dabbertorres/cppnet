#pragma once

#include <algorithm>
#include <coroutine>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "coro/task.hpp"
#include "io/io.hpp"
#include "io/reader.hpp"
#include "io/writer.hpp"

namespace test
{

// started awaits t in a coroutine of its own, started at once, whose task it returns: finished (is_ready()) once t has,
// at once or when what it waits for comes (see fed_reader and held_writer).
template<typename T>
net::coro::task<T> started(net::coro::task<T> t)
{
    auto awaiting = [](net::coro::task<T> inner) -> net::coro::task<T>
    { co_return co_await std::move(inner); }(std::move(t));
    awaiting.get_handle().resume();
    return awaiting;
}

// fed_reader is a reader whose reads wait, as a socket's do (as an operation, which resumes the coroutine awaiting it),
// until the test feeds it: a read finishes at once with what's been fed (or the end, once end() is called), and
// otherwise waits for the next feed() (or end()), which finishes it with what fits. One read waits at a time.
class fed_reader final : public net::io::reader
{
public:
    net::coro::task<net::io::result> read(std::span<std::byte> data) override
    {
        asked.push_back(data.size());
        if (data.empty() || has_more() || ended) return net::coro::ready_task<net::io::result>(take(data));

        if (!op.try_claim())
        {
            return net::coro::ready_task<net::io::result>(
                net::io::result{.err = std::make_error_code(std::errc::device_or_resource_busy)});
        }
        op.into = data;
        return net::coro::operation_task(op);
    }

    using net::io::reader::read;

    [[nodiscard]] net::io::handle native_handle() const noexcept override { return -1; }

    // feed adds text to what's to be read, finishing a read that waits.
    void feed(std::string_view text)
    {
        pending.append(text);
        op.finish();
    }

    // end makes reads report the end of the stream, once what's been fed is read, finishing a read that waits.
    void end()
    {
        ended = true;
        op.finish();
    }

    // waiting is whether a read waits.
    [[nodiscard]] bool waiting() const noexcept { return static_cast<bool>(op.waiting); }

    // asked is how much each read asked for, in turn.
    std::vector<std::size_t> asked;

private:
    net::io::result take(std::span<std::byte> data) noexcept
    {
        const auto left  = std::span{pending}.subspan(at);
        const auto count = std::min(data.size(), left.size());
        std::ranges::copy(std::as_bytes(left.first(count)), data.begin());
        at += count;

        if (count == 0 && ended && !data.empty())
            return {.count = 0, .err = make_error_code(net::io::errc::end_of_stream)};
        return {.count = count};
    }

    // has_more is whether something's been fed that isn't read yet.
    [[nodiscard]] bool has_more() const noexcept { return at < pending.size(); }

    class read_op final : public net::coro::operation<net::io::result>
    {
    public:
        explicit read_op(fed_reader* owner) noexcept
            : owner{owner}
        {}

        bool start(std::coroutine_handle<> awaiting) noexcept override
        {
            // (it may have been fed between the read and its await.)
            if (owner->has_more() || owner->ended)
            {
                res = owner->take(into);
                return false;
            }

            waiting = awaiting;
            return true;
        }

        net::io::result result() override
        {
            release();
            return res;
        }

        void cancel() noexcept override
        {
            waiting = nullptr;
            release();
        }

        // finish finishes the read that waits, if one does.
        void finish()
        {
            if (!waiting) return;

            res = owner->take(into);
            std::exchange(waiting, nullptr).resume();
        }

        std::span<std::byte>    into;
        std::coroutine_handle<> waiting{nullptr};

    private:
        fed_reader*     owner;
        net::io::result res{};
    };

    std::string pending; // what's been fed: read up to at
    std::size_t at    = 0;
    bool        ended = false;
    read_op     op{this};
};

// held_writer is a writer whose writes wait, as a socket's do (as an operation), while it has no room: a write writes
// what fits in the room it has (at once, if there's any), and otherwise waits for make_room() (or fail()), which
// finishes it. One write waits at a time.
class held_writer final : public net::io::writer
{
public:
    net::coro::task<net::io::result> write(std::span<const std::byte> data) override
    {
        asked.push_back(data.size());
        if (data.empty() || room > 0) return net::coro::ready_task<net::io::result>(put(data));

        if (!op.try_claim())
        {
            return net::coro::ready_task<net::io::result>(
                net::io::result{.err = std::make_error_code(std::errc::device_or_resource_busy)});
        }
        op.from = data;
        return net::coro::operation_task(op);
    }

    using net::io::writer::write;

    [[nodiscard]] net::io::handle native_handle() const noexcept override { return -1; }

    // make_room lets count more bytes be written, finishing a write that waits.
    void make_room(std::size_t count)
    {
        room += count;
        op.finish({});
    }

    // fail finishes a write that waits with err, having written nothing.
    void fail(std::error_code err) { op.finish(err); }

    // waiting is whether a write waits.
    [[nodiscard]] bool waiting() const noexcept { return static_cast<bool>(op.waiting); }

    // out is what's been written; asked is how much each write asked to write, in turn.
    std::string              out;
    std::vector<std::size_t> asked;

private:
    net::io::result put(std::span<const std::byte> data)
    {
        const auto count = std::min(data.size(), room);
        out.append(reinterpret_cast<const char*>(data.data()), count);
        room -= count;
        return {.count = count};
    }

    class write_op final : public net::coro::operation<net::io::result>
    {
    public:
        explicit write_op(held_writer* owner) noexcept
            : owner{owner}
        {}

        bool start(std::coroutine_handle<> awaiting) noexcept override
        {
            if (owner->room > 0)
            {
                res = owner->put(from);
                return false;
            }

            waiting = awaiting;
            return true;
        }

        net::io::result result() override
        {
            release();
            return res;
        }

        void cancel() noexcept override
        {
            waiting = nullptr;
            release();
        }

        // finish finishes the write that waits, if one does: failing with err, if there's one.
        void finish(std::error_code err)
        {
            if (!waiting) return;

            res = err ? net::io::result{.err = err} : owner->put(from);
            std::exchange(waiting, nullptr).resume();
        }

        std::span<const std::byte> from;
        std::coroutine_handle<>    waiting{nullptr};

    private:
        held_writer*    owner;
        net::io::result res{};
    };

    std::size_t room = 0;
    write_op    op{this};
};

}
