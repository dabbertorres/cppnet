#pragma once

#include <condition_variable>
#include <cstddef>
#include <exception>
#include <functional>
#include <mutex>
#include <utility>
#include <vector>

#include "coro/task.hpp"

namespace net::coro
{

// async_scope owns detached tasks: it keeps count of the tasks spawned into it, destroys each one's coroutine frame
// when it finishes, and can be joined (waited on until it has no tasks left).
//
// Once closed, a scope refuses new tasks. A scope must outlive its tasks, so its destructor closes it and blocks until
// they have all finished.
class async_scope
{
public:
    // Called with any exception that escapes a spawned task.
    using exception_handler = std::function<void(std::exception_ptr)>;

    // Called once the scope is both closed and empty. It is called while holding the scope's lock, so it must not block
    // or call back into the scope.
    using drained_handler = std::function<void()>;

    class join_operation
    {
        friend class async_scope;

        explicit join_operation(async_scope* scope) noexcept
            : scope{scope}
        {}

    public:
        [[nodiscard]] bool await_ready() const noexcept { return scope->empty(); }
        bool               await_suspend(std::coroutine_handle<> awaiting) noexcept;
        void               await_resume() const noexcept {}

    private:
        async_scope* scope;
    };

    // lease counts as one of the scope's tasks while it's held (until it's destroyed, or released): for work that uses
    // what owns the scope, without being one of its tasks (e.g. being another scope's), which joining the scope must
    // still wait for. A lease taken from a closed scope is empty, and holds nothing.
    class lease
    {
    public:
        lease() noexcept = default;

        lease(const lease&)            = delete;
        lease& operator=(const lease&) = delete;

        lease(lease&& other) noexcept
            : scope{std::exchange(other.scope, nullptr)}
        {}

        lease& operator=(lease&& other) noexcept
        {
            if (this != &other)
            {
                release();
                scope = std::exchange(other.scope, nullptr);
            }

            return *this;
        }

        ~lease() { release(); }

        [[nodiscard]] explicit operator bool() const noexcept { return scope != nullptr; }

        // release releases the lease. NOTE: the scope may be destroyed as soon as it has.
        void release() noexcept
        {
            if (auto* held = std::exchange(scope, nullptr)) held->finish();
        }

    private:
        friend class async_scope;

        explicit lease(async_scope* scope) noexcept
            : scope{scope}
        {}

        async_scope* scope = nullptr;
    };

    // By default, an exception that escapes a spawned task calls std::terminate().
    explicit async_scope(exception_handler on_exception = {}, drained_handler on_drained = {});

    async_scope(const async_scope&)            = delete;
    async_scope& operator=(const async_scope&) = delete;

    async_scope(async_scope&&)            = delete;
    async_scope& operator=(async_scope&&) = delete;

    ~async_scope();

    // spawn starts task on the calling thread, where it runs until it first suspends. If the scope is closed, the task
    // is destroyed without being run, and false is returned.
    bool spawn(task<> task);

    // acquire takes a lease on the scope (see lease): an empty one if the scope is closed.
    [[nodiscard]] lease acquire() noexcept;

    // close stops the scope from accepting new tasks (and leases). Tasks already in it are unaffected.
    void close() noexcept;

    [[nodiscard]] bool        closed() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] bool        empty() const noexcept { return size() == 0; }

    // join completes once the scope is empty. If it isn't empty yet, the awaiting coroutine is resumed on the thread
    // that finishes the scope's last task.
    [[nodiscard]] join_operation join() noexcept { return join_operation{this}; }

    // join_blocking blocks the calling thread until the scope is empty. It must not be called from one of the scope's
    // own tasks, which would then wait for itself.
    void join_blocking() noexcept;

private:
    // detached is the coroutine that owns a spawned task, and destroys itself when done.
    //
    // NOTE: it starts suspended, and is resumed once run() has returned. If it started eagerly, it could finish (and be
    //       destroyed) on another thread while run() is still returning, which GCC's generated code doesn't allow for.
    struct detached
    {
        struct promise_type
        {
            promise_type(async_scope* scope, const task<>& /*unused*/) noexcept
                : scope{scope}
            {}

            detached get_return_object() noexcept { return {std::coroutine_handle<promise_type>::from_promise(*this)}; }

            std::suspend_always initial_suspend() noexcept { return {}; }

            auto final_suspend() noexcept
            {
                struct awaitable
                {
                    [[nodiscard]] bool await_ready() const noexcept { return false; }

                    void await_suspend(std::coroutine_handle<promise_type> self) noexcept
                    {
                        // destroy the frame before telling the scope, so nothing of the task outlives the scope.
                        auto* owner = self.promise().scope;
                        self.destroy();
                        owner->finish();
                    }

                    void await_resume() const noexcept {}
                };

                return awaitable{};
            }

            void return_void() noexcept {}

            // the body catches everything.
            void unhandled_exception() noexcept { std::terminate(); }

            async_scope* scope;
        };

        std::coroutine_handle<promise_type> handle;
    };

    static detached run(async_scope* scope, task<> task);

    void finish() noexcept;

    exception_handler on_exception;
    drained_handler   on_drained;

    mutable std::mutex                   mu;
    std::condition_variable              drained;
    std::vector<std::coroutine_handle<>> joiners;
    std::size_t                          count     = 0;
    bool                                 is_closed = false;
};

}
