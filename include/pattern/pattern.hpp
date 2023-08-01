#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace net::pattern
{

class parse_exception : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;

private:
    // TODO: provide members for communicating context of error
};

// expr provides a simple pattern matching capability as a limited subset of regular expressions.
class expr
{
public:
    using string      = std::string;
    using string_view = std::string_view;

    explicit expr(const char*);
    explicit expr(const string&);
    explicit expr(string_view);

    expr(const expr&)            = default;
    expr& operator=(const expr&) = default;

    expr(expr&&)            = default;
    expr& operator=(expr&&) = default;

    ~expr() = default;

    static expr parse(string_view);

    [[nodiscard]] bool match(string_view input) const noexcept;

private:
    expr() = default;

    enum class repetition
    {
        once,
        zero_or_one,
        zero_or_more,
        one_or_more,
    };

    struct state_literal
    {
        string literal;

        [[nodiscard]] bool match(string_view str, std::size_t& offset) const noexcept;
    };

    struct state_string_alternation
    {
        std::vector<string> choices;

        [[nodiscard]] bool match(string_view str, std::size_t& offset) const noexcept;
    };

    struct state_char_alternation
    {
        std::vector<char> choices;

        [[nodiscard]] bool match(string_view str, std::size_t& offset) const noexcept;
    };

    struct state_wildcard
    {
        [[nodiscard]] bool match(string_view str, std::size_t& offset) const noexcept;
    };

    using state_types = std::variant<state_literal, state_string_alternation, state_char_alternation, state_wildcard>;

    struct state
    {
        state_types impl;
        repetition  rep;

        [[nodiscard]] bool match(string_view str, std::size_t& offset) const noexcept;
    };

    static void parse(string_view, std::vector<state>&);

    static constexpr bool should_repeat(repetition rep, std::size_t current_matches) noexcept;
    static constexpr bool repetition_satisfied(repetition rep, std::size_t current_matches) noexcept;

    std::vector<state> states;
};

}
