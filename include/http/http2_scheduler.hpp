#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

#include "http/http2.hpp"
#include "http/priority.hpp"

// HTTP/2's scheduling of streams' DATA by their priority (RFC 9218; docs/plans/http2.md, stage 5's tenth part: the
// server's responses; and stage 6's seventh: the client's request bodies), without I/O: which of the streams waiting to
// send DATA sends the next frame. Not for applications.
namespace net::http::detail
{

// scheduler_node is a stream's place in an http2_scheduler, and its priority: the stream holds it (a connection's
// streams derive from it), and the scheduler links its nodes through it, so a stream joining the scheduler allocates
// nothing. It's guarded as its scheduler is.
class scheduler_node
{
public:
    explicit scheduler_node(http2::stream_id stream) noexcept
        : key{stream}
    {}

    scheduler_node(const scheduler_node&)            = delete;
    scheduler_node& operator=(const scheduler_node&) = delete;
    scheduler_node(scheduler_node&&)                 = delete;
    scheduler_node& operator=(scheduler_node&&)      = delete;
    ~scheduler_node()                                = default;

    [[nodiscard]] http2::stream_id stream() const noexcept { return key; }

    // set_stream sets the stream's id, while it isn't queued: for a stream made before its id's known (a client's,
    // whose head is made before the stream's opened).
    void set_stream(http2::stream_id stream) noexcept { key = stream; }

    // current_priority is the stream's priority, which http2_scheduler::update() sets.
    [[nodiscard]] priority current_priority() const noexcept { return prio; }

    // queued is whether the stream waits in the scheduler.
    [[nodiscard]] bool queued() const noexcept { return is_queued; }

private:
    friend class http2_scheduler;

    http2::stream_id key;
    priority         prio;
    bool             is_queued = false;

    // the node's neighbours in its level's list (circular, so a node alone is its own).
    scheduler_node* prev = nullptr;
    scheduler_node* next = nullptr;
};

// http2_scheduler orders the streams that wait to send DATA (Decided questions 20 and 21 of stage 5, Go's):
// - the most urgent level (urgency 0 first) with a stream that's ready (as next()'s caller says: it has a window) goes
//   first; less urgent ones wait until it has none ready, as RFC 9218 recommends (section 10 leaves starvation of the
//   less urgent to the server's judgment, and Go lets them starve too);
// - within a level, its non-incremental streams go one at a time, by stream id (the order the client made its
//   requests), the first ready one staying first until it leaves the scheduler (its write is all sent); its
//   incremental streams take turns, a frame each (a ring);
// - when both kinds have a stream ready, they take turns a frame at a time, so neither holds the other up (section
//   10's examples of starvation within an urgency): the kind whose turn it is goes, if it has one ready, and passes the
//   turn; otherwise the other goes, and the turn stays.
//
// It isn't thread-safe: its user (the connection, under its lock) serializes calls into it. Nothing it does allocates.
class http2_scheduler
{
public:
    static constexpr std::size_t urgencies = 8;

    http2_scheduler() = default;

    http2_scheduler(const http2_scheduler&)            = delete;
    http2_scheduler& operator=(const http2_scheduler&) = delete;
    http2_scheduler(http2_scheduler&&)                 = delete;
    http2_scheduler& operator=(http2_scheduler&&)      = delete;
    ~http2_scheduler()                                 = default;

    // add queues a stream that isn't queued, at its priority: a non-incremental one among its level's by its id, an
    // incremental one last in its level's ring (just before the one that goes next).
    void add(scheduler_node& n) noexcept;

    // remove takes a stream out, if it's queued.
    void remove(scheduler_node& n) noexcept;

    // update sets a stream's priority: if it's queued and the priority changed, it moves (as add() places it, so an
    // incremental one goes to the end of its new ring).
    void update(scheduler_node& n, priority p) noexcept;

    // next is the stream to send the next frame (and the scheduler moves on as if it has: its level's ring past it, or
    // the turn to the other kind), among those for which ready(const scheduler_node&) is true; nullptr if none is.
    template<typename Ready>
    [[nodiscard]] scheduler_node* next(Ready ready);

    [[nodiscard]] bool        empty() const noexcept { return count == 0; }
    [[nodiscard]] std::size_t size() const noexcept { return count; }

    // has_urgency is whether any stream of an urgency is queued (ready or not).
    [[nodiscard]] bool has_urgency(std::uint8_t urgency) const noexcept
    {
        const auto& l = levels[index_of(urgency)];
        return l.ordered != nullptr || l.ring != nullptr;
    }

private:
    struct level
    {
        scheduler_node* ordered          = nullptr; // the non-incremental streams, by id (a list's first)
        scheduler_node* ring             = nullptr; // the incremental streams, from the one that goes next
        bool            incremental_turn = true; // which kind goes first when both have a stream ready (Go's starts so)
    };

    // index_of is an urgency's level's index (an urgency past 7, which parse_priority() never makes, is 7's); level_of
    // is a priority's level.
    [[nodiscard]] static std::size_t index_of(std::uint8_t urgency) noexcept
    {
        return std::min<std::size_t>(urgency, urgencies - 1);
    }
    [[nodiscard]] level& level_of(priority p) noexcept { return levels[index_of(p.urgency)]; }

    // first_ready is the first node from first (around its list) that's ready, or nullptr.
    template<typename Ready>
    static scheduler_node* first_ready(scheduler_node* first, Ready& ready);

    std::array<level, urgencies> levels{};
    std::size_t                  count = 0;
};

template<typename Ready>
scheduler_node* http2_scheduler::first_ready(scheduler_node* first, Ready& ready)
{
    if (first == nullptr) return nullptr;

    auto* n = first;
    do
    {
        if (ready(static_cast<const scheduler_node&>(*n))) return n;
        n = n->next;
    }
    while (n != first);
    return nullptr;
}

template<typename Ready>
scheduler_node* http2_scheduler::next(Ready ready)
{
    if (count == 0) return nullptr;

    for (auto& l : levels)
    {
        if (l.ordered == nullptr && l.ring == nullptr) continue;

        // the turn's kind, if it has a stream ready, passing the turn; otherwise the other's, keeping it.
        auto* turn  = l.incremental_turn ? l.ring : l.ordered;
        auto* other = l.incremental_turn ? l.ordered : l.ring;

        auto* n           = first_ready(turn, ready);
        bool  incremental = l.incremental_turn;
        if (n != nullptr)
        {
            l.incremental_turn = !l.incremental_turn;
        }
        else
        {
            n           = first_ready(other, ready);
            incremental = !incremental;
        }
        if (n == nullptr) continue;

        // the ring moves on a frame at a time; the ordered list's first ready stream stays first.
        if (incremental) l.ring = n->next;
        return n;
    }
    return nullptr;
}

}
