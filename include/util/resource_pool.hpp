#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <semaphore>
#include <type_traits>
#include <utility>
#include <vector>

namespace net::util
{

template<typename T>
class resource_pool
{
public:
    using value_type    = T;
    using pointer_type  = std::unique_ptr<value_type>;
    using ref_type      = std::add_lvalue_reference_t<value_type>;
    using make_function = std::function<pointer_type()>;

    struct borrowed_resource
    {
    public:
        borrowed_resource(const borrowed_resource&)            = delete;
        borrowed_resource& operator=(const borrowed_resource&) = delete;

        borrowed_resource(borrowed_resource&& other) noexcept
            : pool{other.pool}
            , ptr{std::exchange(other.ptr, nullptr)}
        {}

        borrowed_resource& operator=(borrowed_resource&&) noexcept = default;

        ~borrowed_resource()
        {
            if (ptr != nullptr) pool->put(std::exchange(ptr, nullptr));
        }

        auto operator*() noexcept { return ptr.operator*(); }
        auto operator->() noexcept { return ptr.operator->(); }
        auto get() noexcept { return ptr.operator->(); }

    private:
        friend class resource_pool;

        borrowed_resource(resource_pool* pool, pointer_type&& ptr)
            : pool{pool}
            , ptr{std::move(ptr)}
        {}

        resource_pool* pool;
        pointer_type   ptr;
    };

    resource_pool(std::size_t target_size, std::size_t max_size, make_function&& make = std::make_unique<T>)
        : target_size{target_size}
        , max_size{max_size}
        , num_waiting{0}
        , num_borrowed{0}
        , make{make}
    {
        pool.reserve(target_size);
    }

    resource_pool(std::size_t max_size, make_function&& make = std::make_unique<T>)
        : resource_pool(max_size, max_size, make)
    {}

    resource_pool(const resource_pool&)            = delete;
    resource_pool& operator=(const resource_pool&) = delete;

    resource_pool(resource_pool&&) noexcept            = default;
    resource_pool& operator=(resource_pool&&) noexcept = default;

    ~resource_pool() = default;

    [[nodiscard]] borrowed_resource get()
    {
        std::unique_lock lock{mu};
        if (pool.empty())
        {
            if (num_borrowed < max_size)
            {
                ++num_borrowed;
                return {this, make()};
            }

            // hard limit - wait for an available resource
            ++num_waiting;
            resource_available.wait(lock, [this] { return !pool.empty(); });
            --num_waiting;
        }

        ++num_borrowed;

        auto res = std::move(pool.back());
        pool.pop_back();
        return {this, std::move(res)};
    }

    [[nodiscard]] std::optional<borrowed_resource> try_get()
    {
        std::unique_lock lock{mu};
        if (pool.empty()) return std::nullopt;

        ++num_borrowed;

        auto res = std::move(pool.back());
        pool.pop_back();
        return std::make_optional(borrowed_resource{this, std::move(res)});
    }

    void put(pointer_type&& res)
    {
        std::lock_guard lock{mu};

        --num_borrowed;

        // over target size - unless there are active waits, just drop the resource
        if (pool.size() >= target_size && num_waiting == 0)
        {
            return;
        }

        pool.push_back(std::move(res));
        resource_available.notify_one();
    }

    template<typename UseFunc, typename R>
        requires(std::is_invocable_r_v<R, UseFunc, ref_type>)
    auto use(UseFunc&& func) noexcept(std::is_nothrow_invocable_r_v<R, UseFunc, ref_type>) -> R
    {
        auto resource = get();
        if constexpr (std::is_void_v<R>)
        {
            std::invoke(func, resource);
        }
        else
        {
            auto ret = std::invoke(func, resource);
            return ret;
        }
    }

private:
    std::size_t target_size;
    std::size_t max_size;

    std::vector<pointer_type> pool;
    std::atomic<std::size_t>  num_waiting;
    std::atomic<std::size_t>  num_borrowed;
    make_function             make;
    std::mutex                mu;
    std::condition_variable   resource_available;
};

}
