#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

#include "coro/task.hpp"
#include "io.hpp"

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

    virtual coro::task<result> write(std::span<const std::byte> data) = 0;
    inline coro::task<result>  write(std::span<const char> data) { return write(std::as_bytes(data)); }
    inline coro::task<result>  write(std::string_view data) { return write(std::span{data.data(), data.length()}); }
    inline coro::task<result>  write(const std::string& data) { return write(std::span{data.data(), data.length()}); }
    inline coro::task<result>  write(std::byte data) { return write(std::span{&data, 1}); }
    inline coro::task<result>  write(char data) { return write(std::span{&data, 1}); }

    [[nodiscard]] virtual int native_handle() const noexcept = 0;

protected:
    writer() = default;
};

inline coro::task<result> write_all(writer& out) { co_return {}; }

template<typename... Writes>
inline coro::task<result> write_all(writer& out, std::span<const std::byte> data, Writes&&... writes)
{
    auto res = co_await out.write(data);
    if (res.err) co_return res;

    std::size_t total = res.count;
    res               = co_await write_all(out, std::forward<Writes>(writes)...);
    total += res.count;

    co_return {.count = total, .err = res.err};
}

template<typename... Writes>
inline coro::task<result> write_all(writer& out, std::span<const char> data, Writes&&... writes)
{
    auto res = co_await out.write(data);
    if (res.err) co_return res;

    std::size_t total = res.count;
    res               = co_await write_all(out, std::forward<Writes>(writes)...);
    total += res.count;

    co_return {.count = total, .err = res.err};
}

template<typename... Writes>
inline coro::task<result> write_all(writer& out, std::string_view data, Writes&&... writes)
{
    auto res = co_await out.write(data);
    if (res.err) co_return res;

    std::size_t total = res.count;
    res               = co_await write_all(out, std::forward<Writes>(writes)...);
    total += res.count;

    co_return {.count = total, .err = res.err};
}

template<typename... Writes>
inline coro::task<result> write_all(writer& out, const std::string& data, Writes&&... writes)
{
    auto res = co_await out.write(data);
    if (res.err) co_return res;

    std::size_t total = res.count;
    res               = co_await write_all(out, std::forward<Writes>(writes)...);
    total += res.count;

    co_return {.count = total, .err = res.err};
}

template<typename... Writes>
inline coro::task<result> write_all(writer& out, std::byte data, Writes&&... writes)
{
    auto res = co_await out.write(data);
    if (res.err) co_return res;

    std::size_t total = res.count;
    res               = co_await write_all(out, std::forward<Writes>(writes)...);
    total += res.count;

    co_return {.count = total, .err = res.err};
}

template<typename... Writes>
inline coro::task<result> write_all(writer& out, char data, Writes&&... writes)
{
    auto res = co_await out.write(data);
    if (res.err) co_return res;

    std::size_t total = res.count;
    res               = co_await write_all(out, std::forward<Writes>(writes)...);
    total += res.count;

    co_return {.count = total, .err = res.err};
}

template<typename Call, typename... Writes>
    requires(std::is_invocable_r_v<coro::task<result>, Call, writer&>)
inline coro::task<result> write_all(writer& out, Call&& call, Writes&&... writes)
{
    auto res = co_await std::invoke(std::forward<Call>(call), out);
    if (res.err) co_return res;

    std::size_t total = res.count;
    res               = co_await write_all(out, std::forward<Writes>(writes)...);
    total += res.count;

    co_return {.count = total, .err = res.err};
}

}
