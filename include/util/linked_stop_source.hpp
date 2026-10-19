#pragma once

#include <stop_token>

namespace net::util
{

// linked_stop_source is a std::stop_source that's also stopped when its parent token is: e.g. a token of a middleware's
// own for the handler it wraps (as http::timeout makes), stopped by a deadline as well as by whatever stops the
// request's. Requesting stop on it doesn't stop the parent.
//
// It's a std::stop_source (which allocates its shared state, as std::stop_source's default constructor does) and a
// std::stop_callback on the parent, which requests stop on it. Stop callbacks run inline, on whichever thread requests
// stop (on the parent, here), or in the constructor if the parent was stopped already: so the source's own callbacks
// (e.g. those of waits given its token) run there too, and must be quick, and not block, as any stop callback.
//
// It can't be copied or moved, as the callback refers to it. Its tokens stay valid after it's destroyed (as any
// std::stop_source's), but are no longer stopped by the parent: destroying it deregisters the callback (waiting for it,
// if it's running on another thread).
class linked_stop_source
{
public:
    // A parent that can't be stopped (e.g. a default-constructed token) links to nothing: the source is stopped only by
    // request_stop().
    explicit linked_stop_source(const std::stop_token& parent)
        : link{parent, stopper{&source}}
    {}

    linked_stop_source(const linked_stop_source&)            = delete;
    linked_stop_source& operator=(const linked_stop_source&) = delete;

    linked_stop_source(linked_stop_source&&)            = delete;
    linked_stop_source& operator=(linked_stop_source&&) = delete;

    ~linked_stop_source() = default;

    [[nodiscard]] std::stop_token get_token() const noexcept { return source.get_token(); }

    // request_stop stops the source (not its parent), returning whether this call did (as std::stop_source's).
    bool request_stop() noexcept { return source.request_stop(); }

    [[nodiscard]] bool stop_requested() const noexcept { return source.stop_requested(); }

private:
    // stopper is the callback on the parent.
    struct stopper
    {
        std::stop_source* target;

        void operator()() const noexcept { target->request_stop(); }
    };

    // NOTE: declared before link, which refers to it: so it's made first (the callback runs in link's constructor if
    //       the parent was stopped already), and destroyed last.
    std::stop_source            source;
    std::stop_callback<stopper> link;
};

}
