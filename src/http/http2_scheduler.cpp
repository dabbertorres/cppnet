#include "http/http2_scheduler.hpp"

#include "http/priority.hpp"

namespace net::http::detail
{

void http2_scheduler::add(scheduler_node& n) noexcept
{
    if (n.is_queued) return;

    auto& l     = level_of(n.prio);
    auto& first = n.prio.incremental ? l.ring : l.ordered;
    n.is_queued = true;
    ++count;

    if (first == nullptr)
    {
        n.prev = &n;
        n.next = &n;
        first  = &n;
        return;
    }

    // the ring's end is just before its first; the ordered list's place is after the last with a lower id, looked for
    // from its end, as a stream that joins usually has the highest id (requests come in order).
    scheduler_node* after = first->prev;
    if (!n.prio.incremental)
    {
        while (after->key > n.key && after != first) after = after->prev;
        if (after->key > n.key) after = nullptr; // (after is first: n goes before it)
    }

    // n goes after after, or, if it's null, before first (the end of the circle), becoming the first.
    auto* before = after != nullptr ? after->next : first;
    auto* prev   = after != nullptr ? after : first->prev;
    n.prev       = prev;
    n.next       = before;
    prev->next   = &n;
    before->prev = &n;
    if (after == nullptr) first = &n;
}

void http2_scheduler::remove(scheduler_node& n) noexcept
{
    if (!n.is_queued) return;

    auto& l     = level_of(n.prio);
    auto& first = n.prio.incremental ? l.ring : l.ordered;
    n.is_queued = false;
    --count;

    if (n.next == &n)
    {
        first = nullptr;
    }
    else
    {
        n.prev->next = n.next;
        n.next->prev = n.prev;

        // (the ring's next to go is then the one after it.)
        if (first == &n) first = n.next;
    }
    n.prev = nullptr;
    n.next = nullptr;
}

void http2_scheduler::update(scheduler_node& n, priority p) noexcept
{
    if (n.prio == p) return;
    if (!n.is_queued)
    {
        n.prio = p;
        return;
    }

    remove(n);
    n.prio = p;
    add(n);
}

}
