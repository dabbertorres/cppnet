#pragma once

#include <cstddef>
#include <string_view>

#include "coro/task.hpp"

#include "io.hpp"

#include "io/aio/scheduler.hpp"

namespace net::io
{

class writer
{
public:
    writer(const writer&)                     = default;
    writer& operator=(const writer&) noexcept = default;

    writer(writer&&) noexcept            = default;
    writer& operator=(writer&&) noexcept = default;

    virtual ~writer() = default;

    virtual result write(const std::byte* data, std::size_t length) = 0;

    inline result write(const char* data, std::size_t length)
    {
        return write(reinterpret_cast<const std::byte*>(data), length);
    }
    inline result write(std::string_view data) { return write(data.data(), data.size()); }

    inline result write(std::byte data) { return write(&data, 1); }
    inline result write(char data) { return write(&data, 1); }

    virtual coro::task<io::result> write(io::aio::scheduler& scheduler, const std::byte* data, std::size_t length) = 0;

    inline coro::task<io::result> write(io::aio::scheduler& scheduler, const char* data, std::size_t length)
    {
        return write(scheduler, reinterpret_cast<const std::byte*>(data), length);
    }

    [[nodiscard]] virtual int native_handle() const noexcept = 0;

protected:
    writer() = default;
};

inline result write_all(writer& /*out*/) { return {}; }

template<typename... Writes>
inline result write_all(writer& out, const std::byte* data, std::size_t length, Writes&&... writes)
{
    auto res = out.write(data, length);
    if (res.err) return res;

    std::size_t total = res.count;
    res               = write_all(out, std::forward<Writes>(writes)...);
    total += res.count;

    return {.count = total, .err = res.err};
}

template<typename... Writes>
inline result write_all(writer& out, const char* data, std::size_t length, Writes&&... writes)
{
    auto res = out.write(data, length);
    if (res.err) return res;

    std::size_t total = res.count;
    res               = write_all(out, std::forward<Writes>(writes)...);
    total += res.count;

    return {.count = total, .err = res.err};
}

template<typename... Writes>
inline result write_all(writer& out, std::string_view data, Writes&&... writes)
{
    auto res = out.write(data);
    if (res.err) return res;

    std::size_t total = res.count;
    res               = write_all(out, std::forward<Writes>(writes)...);
    total += res.count;

    return {.count = total, .err = res.err};
}

template<typename... Writes>
inline result write_all(writer& out, std::byte data, Writes&&... writes)
{
    auto res = out.write(data);
    if (res.err) return res;

    std::size_t total = res.count;
    res               = write_all(out, std::forward<Writes>(writes)...);
    total += res.count;

    return {.count = total, .err = res.err};
}

template<typename... Writes>
inline result write_all(writer& out, char data, Writes&&... writes)
{
    auto res = out.write(data);
    if (res.err) return res;

    std::size_t total = res.count;
    res               = write_all(out, std::forward<Writes>(writes)...);
    total += res.count;

    return {.count = total, .err = res.err};
}

template<typename Call, typename... Writes>
    requires(std::is_invocable_r_v<result, Call, writer&>)
inline result write_all(writer& out, Call&& call, Writes&&... writes)
{
    auto res = std::invoke(std::forward<Call>(call), out);
    if (res.err) return res;

    std::size_t total = res.count;
    res               = write_all(out, std::forward<Writes>(writes)...);
    total += res.count;

    return {.count = total, .err = res.err};
}

}
