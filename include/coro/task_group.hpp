#pragma once

#include <atomic>
#include <concepts>
#include <coroutine>
#include <cstddef>
#include <iterator>
#include <list>
#include <mutex>
#include <utility>
#include <vector>

#include "coro/task.hpp"

namespace net::coro
{

// SameAsAny evaluates to true if type T is the same as any of Others types.
// See std::same_as for how type sameness is evaluated.
template<typename T, typename... Others>
concept SameAsAny = (std::same_as<T, Others> || ...);

template<typename T>
concept Awaiter = requires(T t, std::coroutine_handle<> handle)
// clang-format off
{
    { t.await_ready() }         -> std::same_as<bool>;
    { t.await_suspend(handle) } -> SameAsAny<void, bool, std::coroutine_handle<>>;
    { t.await_resume() };
};
// clang-format on

template<typename T>
concept Executor = requires(T t, std::coroutine_handle<> handle)
// clang-format off
{
    { t.schedule() }     -> Awaiter;
    { t.yield() }        -> Awaiter;
    { t.resume(handle) } -> std::same_as<void>;
};
// clang-format on

// task_group maintains the lifetime of tasks that have shared dependent lifetimes.
class task_group
{
public:
    task_group(std::size_t reserve_size = 8, double growth_factor = 2.0)
        : num_live_tasks(0)
        , growth_factor(growth_factor)

    {
        tasks.reserve(reserve_size);
        for (auto i = 0U; i < reserve_size; ++i) tasks_indices.emplace_back(i);
        free_pos = tasks_indices.begin();
    }

    task_group(const task_group&)            = delete;
    task_group& operator=(const task_group&) = delete;

    task_group(task_group&&) noexcept            = delete;
    task_group& operator=(task_group&&) noexcept = delete;

    // NOTE: the destructor will hang the current thread while waiting for tasks to complete.
    ~task_group()
    {
        while (!empty()) collect_garbage();
    }

    template<Executor E>
    void start(task<void>&& user_task, E* executor)
    {
        num_live_tasks.fetch_add(1, std::memory_order::relaxed);

        std::scoped_lock lock{mutex};

        collect_garbage_internal();

        if (free_pos == tasks_indices.end()) free_pos = grow();

        auto index   = *free_pos;
        tasks[index] = make_cleanup_task(std::move(user_task), free_pos, executor);

        std::advance(free_pos, 1);

        tasks[index].resume();
    }

    std::size_t collect_garbage()
    {
        std::scoped_lock lock{mutex};
        return collect_garbage_internal();
    }

    [[nodiscard]] std::size_t tasks_awaiting_deletion() const
    {
        std::atomic_thread_fence(std::memory_order::acquire);
        return tasks_to_delete.size();
    }

    [[nodiscard]] bool no_tasks_awaiting_deletion() const
    {
        std::atomic_thread_fence(std::memory_order::acquire);
        return tasks_to_delete.empty();
    }

    [[nodiscard]] std::size_t size() const { return num_live_tasks.load(std::memory_order::relaxed); }
    [[nodiscard]] bool        empty() const { return size() == 0; }

    [[nodiscard]] std::size_t capacity() const
    {
        std::atomic_thread_fence(std::memory_order::acquire);
        return tasks.size();
    }

    template<Executor E>
    task<void> collect_garbage_until_empty(E* executor)
    {
        while (!empty())
        {
            collect_garbage();
            co_await executor->yield();
        }
    }

private:
    using task_list     = std::list<std::size_t>;
    using task_position = task_list::iterator;

    task_position grow()
    {
        auto last_pos = std::prev(tasks_indices.end());
        auto new_size = static_cast<std::size_t>(static_cast<double>(tasks.size()) * growth_factor);

        for (auto i = tasks.size(); i < new_size; ++i) tasks_indices.emplace_back(i);

        tasks.resize(new_size);

        return std::next(last_pos);
    }

    std::size_t collect_garbage_internal()
    {
        if (!tasks_to_delete.empty())
        {
            for (const auto& pos : tasks_to_delete)
            {
                tasks_indices.splice(tasks_indices.end(), tasks_indices, pos);
                tasks[*pos].destroy();
            }

            std::size_t deleted = tasks_to_delete.size();
            tasks_to_delete.clear();
            return deleted;
        }

        return 0;
    }

    template<Executor E>
    task<void> make_cleanup_task(task<void> user_task, task_position pos, E* executor)
    {
        co_await executor->schedule();

        // TODO: might need to wrap this in a try ... catch
        co_await user_task;

        std::scoped_lock lock{mutex};
        tasks_to_delete.push_back(pos);

        num_live_tasks.fetch_sub(1, std::memory_order::relaxed);
        co_return;
    }

    std::mutex                 mutex;
    std::atomic<std::size_t>   num_live_tasks;
    std::vector<task<void>>    tasks; // maintains task lifetimes
    task_list                  tasks_indices;
    std::vector<task_position> tasks_to_delete;
    task_position              free_pos; // first free position in tasks_indices.
    double                     growth_factor;
};

}
