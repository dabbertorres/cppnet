#pragma once

#include <algorithm>
#include <any>
#include <cstddef>
#include <iterator>
#include <type_traits>
#include <typeindex>
#include <typeinfo>
#include <utility>
#include <vector>

namespace net::util
{

// type_map holds at most one value of each type, looked up by its type: e.g. request-scoped values that middleware
// passes to handlers, which don't know about each other. Values must be copyable.
//
// NOTE: it's meant for a few values, which it searches linearly.
class type_map
{
public:
    // emplace constructs a T from args, replacing any T already there, and returns it.
    template<typename T, typename... Args>
        requires std::is_same_v<T, std::decay_t<T>> && std::is_copy_constructible_v<T>
    T& emplace(Args&&... args)
    {
        auto iter = find(typeid(T));
        if (iter == values.end())
        {
            values.emplace_back(std::type_index{typeid(T)}, std::any{});
            iter = std::prev(values.end());
        }

        return iter->second.template emplace<T>(std::forward<Args>(args)...);
    }

    // get returns the T, or nullptr if there isn't one.
    template<typename T>
    [[nodiscard]] T* get() noexcept
    {
        auto iter = find(typeid(T));
        return iter == values.end() ? nullptr : std::any_cast<T>(&iter->second);
    }

    template<typename T>
    [[nodiscard]] const T* get() const noexcept
    {
        auto iter = std::ranges::find(values, std::type_index{typeid(T)}, &entry::first);
        return iter == values.end() ? nullptr : std::any_cast<T>(&iter->second);
    }

    // erase removes the T, returning whether there was one.
    template<typename T>
    bool erase() noexcept
    {
        auto iter = find(typeid(T));
        if (iter == values.end()) return false;

        values.erase(iter);
        return true;
    }

    // clear removes every value.
    void clear() noexcept { values.clear(); }

    [[nodiscard]] std::size_t size() const noexcept { return values.size(); }
    [[nodiscard]] bool        empty() const noexcept { return values.empty(); }

private:
    using entry = std::pair<std::type_index, std::any>;

    std::vector<entry>::iterator find(const std::type_info& type) noexcept
    {
        return std::ranges::find(values, std::type_index{type}, &entry::first);
    }

    std::vector<entry> values;
};

}
