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

    virtual result write(std::span<const std::byte> data) = 0;

    inline result write(std::span<const char> data) { return write(std::as_bytes(data)); }
    inline result write(std::string_view data) { return write(std::span{data.data(), data.length()}); }
    inline result write(const std::string& data) { return write(std::span{data.data(), data.length()}); }

    inline result write(std::byte data) { return write(std::span{&data, 1}); }
    inline result write(char data) { return write(std::span{&data, 1}); }

    virtual coro::task<result> co_write(std::span<const std::byte> data)
    {
        // defaults to a synchronous read
        co_return write(data);
    }

    inline coro::task<result> co_write(std::span<const char> data) { return co_write(std::as_bytes(data)); }

    inline coro::task<result> co_write(std::string_view data)
    {
        return co_write(std::span{data.data(), data.length()});
    }

    inline coro::task<result> co_write(const std::string& data)
    {
        return co_write(std::span{data.data(), data.length()});
    }

    inline coro::task<result> co_write(std::byte data) { return co_write(std::span{&data, 1}); }
    inline coro::task<result> co_write(char data) { return co_write(std::span{&data, 1}); }

    [[nodiscard]] virtual int native_handle() const noexcept = 0;

protected:
    writer() = default;
};

inline result write_all(writer& /*out*/) { return {}; }

template<typename... Writes>
inline result write_all(writer& out, std::span<const std::byte> data, Writes&&... writes)
{
    auto res = out.write(data);
    if (res.err) return res;

    std::size_t total = res.count;
    res               = write_all(out, std::forward<Writes>(writes)...);
    total += res.count;

    return {.count = total, .err = res.err};
}

template<typename... Writes>
inline result write_all(writer& out, std::span<const char> data, Writes&&... writes)
{
    auto res = out.write(data);
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
inline result write_all(writer& out, const std::string& data, Writes&&... writes)
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
