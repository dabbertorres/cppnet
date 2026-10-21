#include "http/http2_scheduler.hpp"

#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "http/http2.hpp"
#include "http/priority.hpp"

using net::http::priority;
using net::http::detail::http2_scheduler;
using net::http::detail::scheduler_node;
using net::http::http2::stream_id;

namespace
{

// streams are a scheduler's streams, each with frames to send, and the ones that aren't ready (have no window).
struct streams
{
    http2_scheduler                                      sched;
    std::map<stream_id, std::unique_ptr<scheduler_node>> nodes;
    std::map<stream_id, int>                             frames; // left to send, while queued
    std::set<stream_id>                                  blocked;

    // add makes a stream with frames to send, and queues it.
    scheduler_node& add(stream_id id, priority p, int count = 1)
    {
        auto& n = *nodes.emplace(id, std::make_unique<scheduler_node>(id)).first->second;
        sched.update(n, p);
        sched.add(n);
        frames[id] = count;
        return n;
    }

    scheduler_node& node(stream_id id) { return *nodes.at(id); }

    // take is the streams of the next count frames (as many as there are, if count is 0), as "1 3 1", each stream
    // leaving once it's sent all its frames.
    std::string take(int count = 0)
    {
        std::string out;
        for (int i = 0; count == 0 || i < count; ++i)
        {
            auto* n = sched.next([this](const scheduler_node& s) { return !blocked.contains(s.stream()); });
            if (n == nullptr) break;

            if (!out.empty()) out += ' ';
            out += std::to_string(n->stream());
            if (--frames[n->stream()] == 0) sched.remove(*n);
        }
        return out;
    }
};

constexpr priority urgent{.urgency = 0};
constexpr priority usual{.urgency = 3};
constexpr priority bulk{.urgency = 7};
constexpr priority usual_incremental{.urgency = 3, .incremental = true};

}

TEST_CASE("an empty HTTP/2 scheduler has no next stream", "[http][http2][scheduler]")
{
    streams s;
    CHECK(s.sched.empty());
    CHECK(s.take().empty());
}

TEST_CASE("the HTTP/2 scheduler sends the most urgent streams first", "[http][http2][scheduler]")
{
    streams s;
    s.add(1, bulk, 2);
    s.add(3, urgent, 2);
    s.add(5, usual, 2);
    s.add(7, priority{.urgency = 0, .incremental = true}, 2);
    CHECK(s.sched.size() == 4);

    // urgency 0's two kinds take turns (incremental first), then 3's, then 7's.
    CHECK(s.take() == "7 3 7 3 5 5 1 1");
    CHECK(s.sched.empty());
}

TEST_CASE("the HTTP/2 scheduler sends non-incremental streams one at a time, by stream id", "[http][http2][scheduler]")
{
    streams s;
    s.add(7, usual, 2);
    s.add(3, usual, 3);
    s.add(9, usual, 1);
    s.add(5, usual, 2);
    s.add(1, usual, 1);
    CHECK(s.take() == "1 3 3 3 5 5 7 7 9");
}

TEST_CASE("the HTTP/2 scheduler's non-incremental stream stays first while it has frames to send, a later one queued "
          "with a lower id going before it",
          "[http][http2][scheduler]")
{
    streams s;
    s.add(5, usual, 3);
    CHECK(s.take(1) == "5");

    // (e.g. a write of 3's queued as 5's goes on)
    s.add(3, usual, 1);
    CHECK(s.take() == "3 5 5");
}

TEST_CASE("the HTTP/2 scheduler's incremental streams take turns, a frame each", "[http][http2][scheduler]")
{
    streams s;
    s.add(1, usual_incremental, 2);
    s.add(3, usual_incremental, 1);
    s.add(5, usual_incremental, 3);
    CHECK(s.take(2) == "1 3");

    // a stream joining goes last in the ring: after 5, which is next.
    s.add(7, usual_incremental, 2);
    CHECK(s.take() == "5 1 7 5 7 5");
}

TEST_CASE("within an urgency, the HTTP/2 scheduler's incremental and non-incremental streams take turns",
          "[http][http2][scheduler]")
{
    streams s;
    s.add(1, usual, 3);
    s.add(3, usual_incremental, 2);
    s.add(5, usual_incremental, 2);
    CHECK(s.take() == "3 1 5 1 3 1 5");
}

TEST_CASE("the HTTP/2 scheduler skips streams that aren't ready", "[http][http2][scheduler]")
{
    SECTION("a non-incremental one: the next by id goes, until it's ready again")
    {
        streams s;
        s.add(1, usual, 2);
        s.add(3, usual, 3);
        s.blocked.insert(1);
        CHECK(s.take(2) == "3 3");
        s.blocked.erase(1);
        CHECK(s.take() == "1 1 3");
    }

    SECTION("an incremental one: the ring goes on without it")
    {
        streams s;
        s.add(1, usual_incremental, 2);
        s.add(3, usual_incremental, 2);
        s.add(5, usual_incremental, 2);
        s.blocked.insert(3);
        CHECK(s.take(3) == "1 5 1");
        s.blocked.erase(3);
        CHECK(s.take() == "3 5 3");
    }

    SECTION("a kind with none ready keeps its turn, and goes first once one is")
    {
        streams s;
        s.add(1, usual, 3);
        s.add(3, usual_incremental, 3);
        s.blocked.insert(3);
        CHECK(s.take(2) == "1 1");
        s.blocked.erase(3);
        CHECK(s.take() == "3 1 3 3");
    }

    SECTION("a less urgent level goes while the more urgent has none ready")
    {
        streams s;
        s.add(1, urgent, 2);
        s.add(3, bulk, 2);
        s.blocked.insert(1);
        CHECK(s.take(1) == "3");
        s.blocked.erase(1);
        CHECK(s.take() == "1 1 3");
    }

    SECTION("none ready")
    {
        streams s;
        s.add(1, usual, 2);
        s.add(3, usual_incremental, 2);
        s.blocked = {1, 3};
        CHECK(s.take().empty());
        CHECK(s.sched.size() == 2);
    }
}

TEST_CASE("updating a queued HTTP/2 stream's priority moves it", "[http][http2][scheduler]")
{
    SECTION("to a more urgent level")
    {
        streams s;
        s.add(1, usual, 2);
        s.add(3, bulk, 2);
        s.sched.update(s.node(3), urgent);
        CHECK(s.node(3).current_priority() == urgent);
        CHECK(s.take() == "3 3 1 1");
    }

    SECTION("to the end of its new ring")
    {
        streams s;
        s.add(1, usual_incremental, 2);
        s.add(3, usual_incremental, 2);
        s.add(5, bulk, 2);
        s.sched.update(s.node(5), usual_incremental);
        CHECK(s.take() == "1 3 5 1 3 5");
    }

    SECTION("among its new level's non-incremental streams, by id")
    {
        streams s;
        s.add(3, usual, 2);
        s.add(5, usual, 2);
        s.add(1, usual_incremental, 1);
        s.sched.update(s.node(1), usual);
        CHECK(s.take() == "1 3 3 5 5");
    }

    SECTION("but not if it's the same: the ring's order stays")
    {
        streams s;
        s.add(1, usual_incremental, 2);
        s.add(3, usual_incremental, 2);
        s.sched.update(s.node(1), usual_incremental);
        CHECK(s.take() == "1 3 1 3");
    }

    SECTION("a stream that isn't queued only gets its priority, queued with it later")
    {
        streams s;
        s.add(1, usual, 1);
        scheduler_node n{3};
        s.sched.update(n, urgent);
        CHECK_FALSE(n.queued());
        CHECK(s.sched.size() == 1);

        s.sched.add(n);
        CHECK(n.queued());
        auto* next = s.sched.next([](const scheduler_node&) { return true; });
        CHECK(next == &n);
        s.sched.remove(n);
    }
}

TEST_CASE("removing an HTTP/2 stream from the scheduler", "[http][http2][scheduler]")
{
    SECTION("the ring's next: the one after it goes next")
    {
        streams s;
        s.add(1, usual_incremental, 2);
        s.add(3, usual_incremental, 2);
        s.add(5, usual_incremental, 2);
        CHECK(s.take(1) == "1");
        s.sched.remove(s.node(3));
        CHECK_FALSE(s.node(3).queued());
        CHECK(s.take() == "5 1 5");
    }

    SECTION("an ordered one, the first, the last and the only")
    {
        streams s;
        s.add(1, usual, 1);
        s.add(3, usual, 1);
        s.add(5, usual, 1);
        s.add(7, usual, 1);
        s.sched.remove(s.node(1));
        s.sched.remove(s.node(7));
        CHECK(s.sched.size() == 2);
        CHECK(s.take() == "3 5");

        s.add(9, usual, 1);
        s.sched.remove(s.node(9));
        CHECK(s.sched.empty());
        CHECK(s.take().empty());
    }

    SECTION("one that isn't queued: nothing")
    {
        streams s;
        s.add(1, usual, 1);
        scheduler_node n{3};
        s.sched.remove(n);
        CHECK(s.sched.size() == 1);
        CHECK(s.take() == "1");
    }
}

TEST_CASE("an urgency past 7 is scheduled as 7", "[http][http2][scheduler]")
{
    streams s;
    s.add(1, priority{.urgency = 9}, 1);
    s.add(3, bulk, 1);
    CHECK(s.take() == "1 3");
}
