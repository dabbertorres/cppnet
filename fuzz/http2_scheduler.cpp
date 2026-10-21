// fuzz_http2_scheduler checks HTTP/2's scheduler (http/http2_scheduler.hpp) against a model of it: a script of adds,
// removes, priority updates and next()s (with some streams not ready) over 16 streams, run through both, must pick the
// same streams, in the same order, and agree on which are queued. The model keeps each level's streams in plain
// containers (a sorted vector, and a deque whose front goes next), so it checks the scheduler's intrusive lists: links,
// firsts, the ring's moving on, removals of the next to go, and moves between levels.
//
// The input is the script: each op a byte (its value mod 5) and the bytes it takes.
#include "http/http2_scheduler.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <memory>
#include <print>
#include <span>
#include <vector>

#include "http/http2.hpp"
#include "http/priority.hpp"

namespace
{

using net::http::priority;
using net::http::detail::http2_scheduler;
using net::http::detail::scheduler_node;
using net::http::http2::stream_id;

constexpr std::size_t streams = 16;

[[noreturn]] void fail(const char* what, std::span<const std::uint8_t> input)
{
    std::print(stderr, "PROPERTY FAILED: {}\n  input:", what);
    for (const auto b : input) std::print(stderr, " {:02x}", b);
    std::println(stderr, "");
    std::abort();
}

// model is what the scheduler should do, the simple way.
struct model
{
    struct level
    {
        std::vector<stream_id> ordered; // by id
        std::deque<stream_id>  ring;    // from the one that goes next
        bool                   incremental_turn = true;
    };

    std::array<level, 8>     levels{};
    std::array<priority, 32> prio{};   // by id
    std::array<bool, 32>     queued{}; // by id

    level& level_of(stream_id id) { return levels.at(std::min<std::size_t>(prio.at(id).urgency, 7)); }

    void add(stream_id id)
    {
        if (queued.at(id)) return;
        queued.at(id) = true;

        auto& l = level_of(id);
        if (prio.at(id).incremental) l.ring.push_back(id);
        else l.ordered.insert(std::ranges::upper_bound(l.ordered, id), id);
    }

    void remove(stream_id id)
    {
        if (!queued.at(id)) return;
        queued.at(id) = false;

        auto& l = level_of(id);
        if (prio.at(id).incremental) l.ring.erase(std::ranges::find(l.ring, id));
        else l.ordered.erase(std::ranges::find(l.ordered, id));
    }

    void update(stream_id id, priority p)
    {
        if (prio.at(id) == p) return;
        if (!queued.at(id))
        {
            prio.at(id) = p;
            return;
        }
        remove(id);
        prio.at(id) = p;
        add(id);
    }

    // next is the stream to go next (0: none), ready(id) saying which may.
    template<typename Ready>
    stream_id next(Ready ready)
    {
        for (auto& l : levels)
        {
            const auto in_ring = [&] -> stream_id
            {
                for (std::size_t i = 0; i < l.ring.size(); ++i)
                {
                    if (!ready(l.ring[i])) continue;

                    // the ring moves on past it: what's after it goes first.
                    const auto id = l.ring[i];
                    std::ranges::rotate(l.ring, l.ring.begin() + static_cast<std::ptrdiff_t>(i + 1));
                    return id;
                }
                return 0;
            };
            const auto in_ordered = [&] -> stream_id
            {
                const auto it = std::ranges::find_if(l.ordered, ready);
                return it == l.ordered.end() ? 0 : *it;
            };

            auto id = l.incremental_turn ? in_ring() : in_ordered();
            if (id != 0) l.incremental_turn = !l.incremental_turn;
            else id = l.incremental_turn ? in_ordered() : in_ring();
            if (id != 0) return id;
        }
        return 0;
    }
};

// priority_of reads a priority from a byte: urgencies up to 9 (past 7, scheduled as 7), and incremental.
priority priority_of(std::uint8_t b)
{
    return {.urgency = static_cast<std::uint8_t>((b & 0x7fU) % 10), .incremental = (b & 0x80U) != 0};
}

// script runs a script through the scheduler and the model.
struct script
{
    explicit script(std::span<const std::uint8_t> input)
        : input{input}
    {
        for (std::size_t i = 0; i < streams; ++i) nodes.at(i) = std::make_unique<scheduler_node>((2 * i) + 1);
    }

    script(const script&)            = delete;
    script& operator=(const script&) = delete;
    script(script&&)                 = delete;
    script& operator=(script&&)      = delete;

    // (the nodes mustn't go with the scheduler still linking them.)
    ~script()
    {
        for (auto& n : nodes) sched.remove(*n);
    }

    std::span<const std::uint8_t>                        input;
    std::size_t                                          at = 0;
    std::array<std::unique_ptr<scheduler_node>, streams> nodes;
    http2_scheduler                                      sched;
    model                                                m;

    std::uint8_t    take() { return at < input.size() ? input[at++] : 0; }
    scheduler_node& node(std::uint8_t b) { return *nodes.at(b % streams); }

    void run()
    {
        while (at < input.size())
        {
            step();
            check();
        }
    }

    void step()
    {
        switch (take() % 5)
        {
        case 0:
        {
            auto& n = node(take());
            sched.add(n);
            m.add(n.stream());
            break;
        }
        case 1:
        {
            auto& n = node(take());
            sched.remove(n);
            m.remove(n.stream());
            break;
        }
        case 2:
        {
            auto&      n = node(take());
            const auto p = priority_of(take());
            sched.update(n, p);
            m.update(n.stream(), p);
            if (n.current_priority() != p) fail("update's priority", input);
            break;
        }
        default: nexts(); break;
        }
    }

    // nexts takes some next()s, with some streams not ready (a mask over them), removing what's picked if its bit's set
    // in another.
    void nexts()
    {
        const auto     count = static_cast<std::size_t>((take() % 8) + 1);
        const unsigned mask  = (unsigned{take()} << 8U) | take();
        const unsigned done  = (unsigned{take()} << 8U) | take();

        const auto bit   = [](stream_id id) { return 1U << ((id - 1) / 2); };
        const auto ready = [&](stream_id id) { return (mask & bit(id)) == 0; };
        for (std::size_t i = 0; i < count; ++i)
        {
            const auto* got  = sched.next([&](const scheduler_node& n) { return ready(n.stream()); });
            const auto  want = m.next(ready);
            if ((got == nullptr ? 0 : got->stream()) != want) fail("next()", input);
            if (want == 0) break;
            if ((done & bit(want)) != 0)
            {
                sched.remove(node(static_cast<std::uint8_t>((want - 1) / 2)));
                m.remove(want);
            }
        }
    }

    void check()
    {
        std::size_t queued = 0;
        for (const auto& n : nodes)
        {
            if (n->queued() != m.queued.at(n->stream())) fail("queued()", input);
            if (n->queued()) ++queued;
        }
        if (sched.size() != queued || sched.empty() != (queued == 0)) fail("size()", input);
    }
};

}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    script{
        std::span{data, size}
    }
        .run();
    return 0;
}
