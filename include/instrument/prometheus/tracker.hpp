#pragma once

#include <chrono>
#include <concepts>

namespace net::instrument::prometheus
{

// tracker calls its callback with how long it lived, in (fractional) seconds, when it's destroyed.
//
// NOTE: neither copyable nor movable, so it reports once. Returning one (e.g. from track()) needs neither.
template<std::invocable<std::chrono::duration<double>> Callback, typename Clock = std::chrono::steady_clock>
struct tracker
{
public:
    explicit tracker(Callback&& callback)
        : callback{std::move(callback)}
        , start{Clock::now()}
    {}

    tracker(const tracker&)            = delete;
    tracker& operator=(const tracker&) = delete;

    tracker(tracker&&)            = delete;
    tracker& operator=(tracker&&) = delete;

    ~tracker() { callback(std::chrono::duration<double>{Clock::now() - start}); }

    void reset() noexcept { start = Clock::now(); }

private:
    Callback          callback;
    Clock::time_point start;
};

}
