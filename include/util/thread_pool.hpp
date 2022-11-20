#pragma once

#include <atomic>
#include <concepts>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <type_traits>
#include <vector>

namespace net::util
{

class thread_pool
{
public:
    thread_pool(size_t concurrency = std::thread::hardware_concurrency());

    thread_pool(const thread_pool&)            = delete;
    thread_pool& operator=(const thread_pool&) = delete;

    thread_pool(thread_pool&&) noexcept            = delete;
    thread_pool& operator=(thread_pool&&) noexcept = delete;

    ~thread_pool();

    // num_workers returns the number of work threads managed by the thread_pool.
    [[nodiscard]] size_t num_workers() const;

    // num_running_jobs returns the number of jobs currently running.
    [[nodiscard]] size_t num_running_jobs() const;

    // num_pending_jobs returns the number of jobs not currently running.
    [[nodiscard]] size_t num_pending_jobs() const;

    // cancel clears the job queue, and requests worker threads to exit as soon as possible.
    void cancel();

    // schedule adds func to the work queue, and returns a std::future<R> which can be used
    // to retrieve the result of the job.
    // If func returns void, a std::future<void> is still returned.
    template<std::invocable Func>
    auto schedule(Func&& func) noexcept -> std::future<std::invoke_result_t<Func>>
    {
        using result_type = std::invoke_result_t<Func>;

        if (!running) return std::future<result_type>{};

        auto promise = std::make_shared<std::promise<result_type>>();
        auto future  = promise->get_future();

        std::unique_lock lock(jobs_mu);

        jobs.push(
            [=, func = std::forward<Func>(func)]
            {
                try
                {
                    if constexpr (std::is_void_v<result_type>)
                    {
                        std::invoke(func);
                        promise->set_value();
                    }
                    else
                    {
                        auto ret = std::invoke(func);
                        promise->set_value(ret);
                    }
                }
                catch (...)
                {
                    promise->set_exception(std::current_exception());
                }
            });

        check_for_job.notify_one();

        return future;
    }

private:
    void worker();

    std::vector<std::thread>          threads;
    std::queue<std::function<void()>> jobs;
    mutable std::mutex                jobs_mu;
    std::condition_variable           check_for_job;
    std::atomic<bool>                 running;
    std::atomic<size_t>               working;
};

}
