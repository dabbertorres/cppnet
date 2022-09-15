#pragma once

#include <atomic>
#include <coroutine>
#include <mutex>
#include <thread>
#include <vector>

namespace net::util
{

class thread_pool
{
public:
    thread_pool(size_t concurrency = std::thread::hardware_concurrency());

    thread_pool(const thread_pool&)            = delete;
    thread_pool& operator=(const thread_pool&) = delete;

    thread_pool(thread_pool&&) noexcept;
    thread_pool& operator=(thread_pool&&) noexcept;

    ~thread_pool();

private:
    std::vector<std::thread> threads;
};

}