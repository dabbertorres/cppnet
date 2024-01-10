#pragma once

#include <cstddef>
#include <span>

#include "coro/task.hpp"
#include "io/scheduler.hpp"

#include "io.hpp"

namespace net::io
{

class reader
{
public:
    reader(const reader&)                     = default;
    reader& operator=(const reader&) noexcept = default;

    reader(reader&&) noexcept            = default;
    reader& operator=(reader&&) noexcept = default;

    virtual ~reader() = default;

    virtual result read(std::span<std::byte> data) = 0;
    inline result  read(std::span<char> data) { return read(std::as_writable_bytes(data)); }

    template<typename T, std::size_t N = std::dynamic_extent>
    inline result read(std::span<T, N> data)
    {
        return read(std::as_writable_bytes(data));
    }

    template<typename T>
    inline result read(T& data)
    {
        return read(std::as_writable_bytes(std::span{&data, 1}));
    }

    /* virtual coro::task<io::result> read(scheduler& scheduler, std::byte* data, std::size_t length) = 0; */

    /* inline coro::task<io::result> read(scheduler& scheduler, char* data, std::size_t length) */
    /* { */
    /*     return read(scheduler, reinterpret_cast<std::byte*>(data), length); */
    /* } */

    [[nodiscard]] virtual int native_handle() const noexcept = 0;

protected:
    reader() = default;
};

}
