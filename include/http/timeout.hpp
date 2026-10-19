#pragma once

#include <chrono>
#include <exception>
#include <stop_token>
#include <utility>

#include "coro/task.hpp"
#include "http/request.hpp"
#include "http/response.hpp"
#include "io/scheduler.hpp"
#include "util/linked_stop_source.hpp"

namespace net::http
{

// timeout is a middleware (see http/middleware.hpp) that gives the rest of its chain a deadline, as Go's
// TimeoutHandler does: e.g.
//
//   http::server server{&scheduler, http::chain(instrument::http_metrics{}, http::timeout{&scheduler, 5s}, router)};
//
// For each request, it swaps req.stop for a token of its own (a util::linked_stop_source, so the request's own stops,
// e.g. the client's reset or the server's abort, still reach the rest of the chain through it), stopped at the
// deadline (by the scheduler's call_at(), without a task), and puts the request's token back once the rest returns
// (or throws).
//
// Canceling is cooperative, as the request's own token's is: at the deadline, the rest of the chain is only asked to
// stop (its waits given req.stop end with std::errc::operation_canceled), and the timeout answers once it has returned:
// if the deadline passed, no final response was sent, and the request's own token wasn't stopped (then its client is
// gone, or the server aborted, and the server sends nothing for it), it sends 503 Service Unavailable with an empty
// body (and whatever headers the rest of the chain had set). A response the rest of the chain sent stands. It never
// writes while the rest of the chain might: unlike Go's, it doesn't buffer the response so as to answer at the
// deadline. So a handler that ignores its token delays the 503 until it returns, as it would any response, and holds
// its connection (or its HTTP/2 stream's place) meanwhile. If the rest of the chain throws once the deadline has
// passed, the 503 is still sent (the deadline being the likelier cause, e.g. of a canceled wait turned into an
// exception), and then the exception rethrown (so the server logs it, and closes an HTTP/1.1 connection).
//
// Middleware that records what was sent (instrument::http_metrics, http_tracing) goes outside it, as above: it then
// sees the request's own token, and the 503 that was sent (recorded as any 5xx, an error). Inside it, it would see the
// timeout's token stopped and no response sent yet, and record 499 Client Closed Request, as for a request the client's
// side canceled.
//
// It costs a request a std::stop_source's allocation (the linked one's shared state), and a timer: an entry in the
// scheduler's timers (whose nodes are reused), added and removed under a lock.
class timeout
{
public:
    // sched is the scheduler whose timers stop the rest of the chain after after, from when the request reaches the
    // timeout. It must outlive the requests the timeout handles.
    timeout(io::scheduler* sched, io::clock::duration after) noexcept
        : sched{sched}
        , after{after}
    {}

    template<class Next>
    coro::task<void> operator()(server_request& req, response_writer& resp, Next& next) const
    {
        util::linked_stop_source source{req.stop};

        // NOTE: set by the call, and read once it's canceled (which waits for it, if it's being made).
        bool expired = false;

        auto deadline = sched->call_at(io::clock::now() + after,
                                       [&source, &expired] noexcept
                                       {
                                           expired = true;
                                           source.request_stop();
                                       });

        auto original = std::exchange(req.stop, source.get_token());

        // NOTE: a co_await can't be in a catch block, so a failure is rethrown after it.
        std::exception_ptr failure;
        try
        {
            co_await next(req, resp);
        }
        catch (...)
        {
            failure = std::current_exception();
        }

        req.stop = std::move(original);
        deadline.cancel();

        if (expired && !resp.sent() && !req.stop.stop_requested()) co_await resp.send(status::SERVICE_UNAVAILABLE, 0);

        if (failure != nullptr) std::rethrow_exception(failure);
    }

private:
    io::scheduler*      sched;
    io::clock::duration after;
};

}
