#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

#include "coro/task.hpp"
#include "io/io.hpp"
#include "io/reader.hpp"

namespace net::io
{

// limit_reader reads at most limit bytes of another reader, and then reports the end of the stream (without reading
// any more of it).
//
// By default, the stream must have limit bytes (e.g. a body framed by its length): if it ends before then, the read
// that finds its end fails with std::errc::connection_aborted (as http11::chunked_reader does for a body cut short)
// rather than io::errc::end_of_stream, still with what it read, so a stream cut short isn't taken for all of it. With
// mode::up_to, the limit is only a cap, and the stream may end before it.
class limit_reader : public reader
{
public:
    // mode is what the stream ending before the limit means.
    enum class mode : std::uint8_t
    {
        exact, // it was cut short: std::errc::connection_aborted.
        up_to, // it's the end: io::errc::end_of_stream.
    };

    constexpr limit_reader()
        : limit_reader(nullptr, 0)
    {}

    constexpr limit_reader(std::unique_ptr<reader>&& reader, std::size_t limit, mode how = mode::exact)
        : owned{std::move(reader)}
        , parent{owned.get()}
        , limit{limit}
        , how{how}
    {}

    // reader is borrowed, and must outlive this limit_reader.
    constexpr limit_reader(reader* reader, std::size_t limit, mode how = mode::exact)
        : parent{reader}
        , limit{limit}
        , how{how}
    {}

    coro::task<result> read(std::span<std::byte> data) override;

    using reader::read;

    [[nodiscard]] handle native_handle() const noexcept override { return parent->native_handle(); }

    // call_at_end sets what's called once the limit's reached: by the read that reaches it (before it returns), or now,
    // if it has been already (e.g. a limit of 0).
    void call_at_end(end_callback callback) noexcept
    {
        at_end = callback;
        if (progress >= limit) at_end();
    }

private:
    coro::task<result> count_after(coro::task<result> inner);

    // counted counts what a read of the parent read, and reports the stream ending before the limit as mode says.
    result counted(result res) noexcept;

    std::unique_ptr<reader> owned    = nullptr;
    reader*                 parent   = nullptr;
    std::size_t             limit    = 0;
    std::size_t             progress = 0;
    mode                    how      = mode::exact;
    end_callback            at_end;
};

}
