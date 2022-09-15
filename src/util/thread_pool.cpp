#include "util/thread_pool.hpp"

#include <cstddef>

namespace net::util
{

thread_pool::thread_pool(size_t concurrency)
{
    if (concurrency == 0) concurrency = 4;

    threads.reserve(concurrency);

    for (size_t i = 0; i < concurrency; ++i)
    {
        threads.emplace_back(
            // TODO
        );
    }
}

}
