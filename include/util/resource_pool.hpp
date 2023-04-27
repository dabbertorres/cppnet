#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <type_traits>
#include <vector>

namespace net::util
{

template<typename T, std::size_t TargetSize, std::size_t MaxSize = TargetSize>
    requires(MaxSize >= TargetSize)
class resource_pool
{
public:
    using value_type    = T;
    using pointer_type  = std::unique_ptr<T>;
    using make_function = std::function<pointer_type()>;

    static constexpr std::size_t target_size = TargetSize;
    static constexpr std::size_t max_size    = MaxSize;

    resource_pool(make_function&& make = std::make_unique<T>)
        : num_waiting(0)
        , make(make)
    {
        pool.reserve(target_size);
    }

    resource_pool(const resource_pool&)            = delete;
    resource_pool& operator=(const resource_pool&) = delete;

    resource_pool(resource_pool&&) noexcept            = default;
    resource_pool& operator=(resource_pool&&) noexcept = default;

    ~resource_pool() = default;

    [[nodiscard]] std::size_t available_resources() const noexcept
    {
        std::lock_guard lock(mu);
        return pool.size();
    }

    [[nodiscard]] pointer_type get()
    {
        std::unique_lock lock(mu);
        if (pool.empty())
        {
            if (pool.size() < max_size) return make();

            // hard limit - wait for an available resource
            ++num_waiting;
            resource_available.wait(lock, [this] { return !pool.empty(); });
            --num_waiting;
        }

        auto res = std::move(pool.back());
        pool.pop_back();
        return res;
    }

    [[nodiscard]] std::optional<pointer_type> try_get()
    {
        std::unique_lock lock(mu);
        if (pool.empty()) return std::nullopt;

        auto res = std::move(pool.back());
        pool.pop_back();
        return res;
    }

    void put(pointer_type&& res)
    {
        std::lock_guard lock(mu);

        // over target size - unless there are active waits, just drop the resource
        if (pool.size() >= target_size && num_waiting == 0)
        {
            return;
        }

        pool.push_back(std::move(res));
        resource_available.notify_one();
    }

    template<std::invocable<pointer_type> UseFunc, typename R>
        requires(!std::is_void_v<R> && std::is_invocable_r_v<R, UseFunc, pointer_type>)
    auto use(UseFunc&& func) -> R
    {
        auto resource = get();
        try
        {
            auto ret = std::invoke(func, resource);
            put(resource);
            return ret;
        }
        catch (...)
        {
            put(resource);
            throw;
        }
    }

    template<std::invocable<pointer_type> UseFunc, typename R>
        requires(!std::is_void_v<R> && std::is_invocable_r_v<R, UseFunc, pointer_type>
                 && std::is_nothrow_invocable_v<UseFunc, pointer_type>)
    auto use(UseFunc&& func) noexcept -> R
    {
        auto resource = get();
        auto ret      = std::invoke(func, resource);
        put(resource);
        return ret;
    }

    template<std::invocable<pointer_type> UseFunc>
        requires(std::is_invocable_r_v<void, UseFunc, pointer_type>)
    auto use(UseFunc&& func) -> std::invoke_result_t<UseFunc, pointer_type>
    {
        auto resource = get();
        try
        {
            func(resource);
            put(resource);
        }
        catch (...)
        {
            put(resource);
            throw;
        }
    }

    template<std::invocable<pointer_type> UseFunc>
        requires(std::is_invocable_r_v<void, UseFunc, pointer_type>
                 && std::is_nothrow_invocable_v<UseFunc, pointer_type>)
    auto use(UseFunc&& func) noexcept -> std::invoke_result_t<UseFunc, pointer_type>
    {
        auto resource = get();
        func(resource);
        put(resource);
    }

private:
    std::vector<pointer_type> pool;
    std::atomic<std::size_t>  num_waiting;
    make_function             make;
    mutable std::mutex        mu;
    std::condition_variable   resource_available;
};

}
