#pragma once

#include <algorithm>
#include <cstddef>
#include <iosfwd>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "coro/task.hpp"
#include "io/io.hpp"
#include "io/reader.hpp"

namespace net::io
{

template<typename CharT, typename Traits = std::char_traits<CharT>>
class string_reader : public reader
{
public:
    using string = std::basic_string_view<CharT, Traits>;

    explicit string_reader(string view)
        : view{std::as_bytes(std::span{view})}
    {}

    // (reads never wait, so they're ready tasks: no coroutine.)
    coro::task<result> read(std::span<CharT> data)
    {
        auto [count, err] = take(std::as_writable_bytes(data));
        return coro::ready_task<result>(result{
            .count = count / sizeof(CharT),
            .err   = err,
        });
    }

    coro::task<result> read(std::span<std::byte> data) override { return coro::ready_task<result>(take(data)); }

    [[nodiscard]] handle native_handle() const noexcept override { return 0; }

private:
    // take copies as much as fits in data of what's left.
    result take(std::span<std::byte> data) noexcept
    {
        if (data.empty()) return {.count = 0};
        if (idx == view.size()) return {.count = 0, .err = make_error_code(errc::end_of_stream)};

        auto amount = std::min(view.size() - idx, data.size());
        std::copy_n(view.begin() + idx, amount, data.begin());
        idx += amount;
        return {.count = amount};
    }

    std::span<const std::byte> view;
    std::size_t                idx = 0;
};

// owned_string_reader reads from a string that it owns.
class owned_string_reader final : public reader
{
public:
    explicit owned_string_reader(std::string content)
        : content{std::move(content)}
        , impl{this->content}
    {}

    // NOTE: impl refers to content, which moving would leave behind.
    owned_string_reader(const owned_string_reader&)            = delete;
    owned_string_reader& operator=(const owned_string_reader&) = delete;
    owned_string_reader(owned_string_reader&&)                 = delete;
    owned_string_reader& operator=(owned_string_reader&&)      = delete;

    ~owned_string_reader() override = default;

    coro::task<result> read(std::span<std::byte> data) override { return impl.read(data); }

    using reader::read;

    [[nodiscard]] handle native_handle() const noexcept override { return 0; }

private:
    std::string         content;
    string_reader<char> impl;
};

// shared_string_reader reads from a string that it shares: e.g. readers of one content, made one at a time (as
// client_request::set_body's get_body makes them), without copying it.
class shared_string_reader final : public reader
{
public:
    explicit shared_string_reader(std::shared_ptr<const std::string> content)
        : content{std::move(content)}
        , impl{*this->content}
    {}

    // NOTE: not movable, as owned_string_reader isn't (though content's string stays put).
    shared_string_reader(const shared_string_reader&)            = delete;
    shared_string_reader& operator=(const shared_string_reader&) = delete;
    shared_string_reader(shared_string_reader&&)                 = delete;
    shared_string_reader& operator=(shared_string_reader&&)      = delete;

    ~shared_string_reader() override = default;

    coro::task<result> read(std::span<std::byte> data) override { return impl.read(data); }

    using reader::read;

    [[nodiscard]] handle native_handle() const noexcept override { return 0; }

private:
    std::shared_ptr<const std::string> content;
    string_reader<char>                impl;
};

}
