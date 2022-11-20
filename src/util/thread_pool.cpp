#include "util/thread_pool.hpp"

namespace net::util
{

thread_pool::thread_pool(size_t concurrency)
    : running(true)
{
    if (concurrency == 0) concurrency = 4;

    threads.reserve(concurrency);

    for (size_t i = 0; i < concurrency; ++i)
    {
        threads.emplace_back(&thread_pool::worker, this);
    }
}

thread_pool::~thread_pool()
{
    running = false;
    check_for_job.notify_all();

    for (auto& t : threads)
    {
        if (t.joinable()) t.join();
    }
}

size_t thread_pool::num_workers() const { return threads.size(); }

size_t thread_pool::num_running_jobs() const { return working; }

size_t thread_pool::num_pending_jobs() const
{
    std::unique_lock lock(jobs_mu);
    return jobs.size();
}

void thread_pool::cancel()
{
    running = false;

    std::unique_lock lock(jobs_mu);
    while (jobs.empty()) jobs.pop();
    check_for_job.notify_all();
}

void thread_pool::worker()
{
    while (true)
    {
        std::unique_lock lock(jobs_mu);
        check_for_job.wait(lock, [&] { return !running || !jobs.empty(); });

        if (jobs.empty())
        {
            if (!running) break;
            continue;
        }

        auto job = std::move(jobs.front());
        jobs.pop();

        lock.unlock();

        ++working;
        job();
        --working;
    }
}

}
