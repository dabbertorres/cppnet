#include "util/small_vector.hpp"

#include <exception> // IWYU pragma: keep
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

using net::util::small_vector;

TEST_CASE("holds up to N elements in itself", "[util][small_vector]")
{
    small_vector<std::string, 2> v;
    CHECK(v.empty());

    v.push_back("a");
    v.emplace_back("b");
    CHECK(v.size() == 2);
    CHECK_FALSE(v.spilled());
    CHECK(std::vector<std::string>(v.begin(), v.end()) == std::vector<std::string>{"a", "b"});
}

TEST_CASE("moves to the heap past N elements, keeping them", "[util][small_vector]")
{
    small_vector<std::string, 2> v;
    v.push_back("a");
    v.push_back("b");
    v.push_back("c");

    CHECK(v.spilled());
    CHECK(v.size() == 3);
    CHECK(v.front() == "a");
    CHECK(v[1] == "b");
    CHECK(v.back() == "c");

    // an element of itself, which spilling moves.
    small_vector<std::string, 1> w;
    w.push_back("x");
    w.emplace_back(w.front());
    CHECK(w.size() == 2);
    CHECK(w[1] == "x");
}

TEST_CASE("clear keeps what it allocated", "[util][small_vector]")
{
    small_vector<int, 1> v;
    v.push_back(1);
    v.push_back(2);
    v.clear();

    CHECK(v.empty());
    CHECK(v.spilled());

    v.push_back(3);
    CHECK(v.size() == 1);
    CHECK(v.front() == 3);
}

TEST_CASE("copies compare equal", "[util][small_vector]")
{
    small_vector<int, 2> a;
    a.push_back(1);
    a.push_back(2);
    a.push_back(3);

    const auto b = a;
    CHECK(a == b);

    small_vector<int, 2> c;
    c.push_back(1);
    CHECK_FALSE(a == c);
}
