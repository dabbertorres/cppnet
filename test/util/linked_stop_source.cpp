#include "util/linked_stop_source.hpp"

#include <atomic>
#include <optional>
#include <stop_token>
#include <thread>
#include <type_traits>

#include <catch2/catch_test_macros.hpp>

using net::util::linked_stop_source;

static_assert(!std::is_copy_constructible_v<linked_stop_source> && !std::is_move_constructible_v<linked_stop_source>);

TEST_CASE("a linked stop source is stopped when its parent is", "[util][linked_stop_source]")
{
    std::stop_source   parent;
    linked_stop_source linked{parent.get_token()};

    const auto token = linked.get_token();
    REQUIRE(token.stop_possible());
    CHECK_FALSE(token.stop_requested());

    // its callbacks run inline, on the thread that stopped the parent.
    std::optional<std::thread::id> ran_on;
    std::stop_callback             on_stop{token, [&] { ran_on = std::this_thread::get_id(); }};

    CHECK(parent.request_stop());
    CHECK(token.stop_requested());
    CHECK(linked.stop_requested());
    CHECK(ran_on == std::this_thread::get_id());

    // already stopped.
    CHECK_FALSE(linked.request_stop());
}

TEST_CASE("stopping a linked stop source doesn't stop its parent", "[util][linked_stop_source]")
{
    std::stop_source   parent;
    linked_stop_source linked{parent.get_token()};

    CHECK(linked.request_stop());
    CHECK(linked.get_token().stop_requested());
    CHECK_FALSE(parent.stop_requested());
}

TEST_CASE("a linked stop source whose parent was stopped already is stopped at once", "[util][linked_stop_source]")
{
    std::stop_source parent;
    parent.request_stop();

    linked_stop_source linked{parent.get_token()};
    CHECK(linked.stop_requested());
}

TEST_CASE("a linked stop source whose parent can't be stopped is stopped only by request_stop()",
          "[util][linked_stop_source]")
{
    linked_stop_source linked{std::stop_token{}};

    const auto token = linked.get_token();
    CHECK(token.stop_possible());
    CHECK_FALSE(token.stop_requested());

    CHECK(linked.request_stop());
    CHECK(token.stop_requested());
}

TEST_CASE("a linked stop source's tokens aren't stopped by its parent once it's gone", "[util][linked_stop_source]")
{
    std::stop_source parent;
    std::stop_token  token;

    {
        linked_stop_source linked{parent.get_token()};
        token = linked.get_token();
    }

    parent.request_stop();
    CHECK_FALSE(token.stop_requested());
}

TEST_CASE("a linked stop source's parent may be stopped on another thread", "[util][linked_stop_source]")
{
    std::stop_source   parent;
    linked_stop_source linked{parent.get_token()};

    std::atomic<bool>  seen = false;
    std::stop_callback on_stop{linked.get_token(), [&] { seen.store(true); }};

    std::thread stopper{[&] { parent.request_stop(); }};
    stopper.join();

    CHECK(seen.load());
    CHECK(linked.stop_requested());
}
