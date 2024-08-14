#pragma once

#include <chrono>
#include <concepts>

namespace net::instrument::prometheus
{

template<std::invocable<std::chrono::seconds> Callback, typename Clock = std::chrono::steady_clock>
struct tracker
{
public:
    tracker(Callback&& callback)
        : callback{callback}
        , start{Clock::now()}
    {}

    tracker(const tracker&)            = default;
    tracker& operator=(const tracker&) = default;

    tracker(tracker&&) noexcept            = default;
    tracker& operator=(tracker&&) noexcept = default;

    ~tracker()
    {
        auto end      = Clock::now();
        auto duration = end - start;
        callback(std::chrono::duration_cast<std::chrono::seconds>(duration));
    }

    void reset() noexcept { start = Clock::now(); }

private:
    Callback          callback;
    Clock::time_point start;
};

}
