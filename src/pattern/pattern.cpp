#include "pattern/pattern.hpp"

#include <queue>
#include <stack>

#include "util/string_util.hpp"

using namespace std::string_view_literals;

namespace
{

bool is_operator(char c) noexcept
{
    switch (c)
    {
    case '|': [[fallthrough]];
    case '[': [[fallthrough]];
    case ']': [[fallthrough]];

    case '.': [[fallthrough]];

    case '*': [[fallthrough]];
    case '+': [[fallthrough]];
    case '?': [[fallthrough]];

    case '(': [[fallthrough]];
    case ')': return true;

    default: return false;
    }
}

bool is_operator(net::pattern::expr::string_view str) noexcept
{
    if (str.length() != 1) return false;

    return is_operator(str[0]);
}

}

namespace net::pattern
{

expr::expr(const char* str) { parse(str, states); }
expr::expr(const string& str) { parse(str, states); }
expr::expr(string_view str) { parse(str, states); }

enum class op
{
    // by precedence, weakest -> strongest

    vertical_bar,
    bracket_open,
    bracket_close,

    period,

    asterisk,
    plus,
    question_mark,

    paren_open,
    paren_close,
};

constexpr expr::string_view op_to_char(op op)
{
    switch (op)
    {
    case op::vertical_bar: return "|"sv;
    case op::bracket_open: return "["sv;
    case op::bracket_close: return "]"sv;
    case op::period: return "."sv;
    case op::asterisk: return "*"sv;
    case op::plus: return "+"sv;
    case op::question_mark: return "?"sv;
    case op::paren_open: return "("sv;
    case op::paren_close: return ")"sv;
    }
}

constexpr bool operator<=(op lhs, op rhs) noexcept
{
    switch (lhs)
    {
    case op::vertical_bar: [[fallthrough]];
    case op::bracket_open: [[fallthrough]];
    case op::bracket_close: return true;

    case op::period:
        switch (rhs)
        {
        case op::vertical_bar: [[fallthrough]];
        case op::bracket_open: [[fallthrough]];
        case op::bracket_close: return false;

        default: return true;
        }

    case op::asterisk: [[fallthrough]];
    case op::plus: [[fallthrough]];
    case op::question_mark:
        switch (rhs)
        {
        case op::vertical_bar: [[fallthrough]];
        case op::bracket_open: [[fallthrough]];
        case op::bracket_close: [[fallthrough]];
        case op::period: return false;

        default: return true;
        }

    case op::paren_open: [[fallthrough]];
    case op::paren_close:
        switch (rhs)
        {
        case op::paren_close: [[fallthrough]];
        case op::paren_open: return true;

        default: return false;
        }
    }
}

expr expr::parse(string_view str)
{
    expr ret;
    parse(str, ret.states);
    return ret;
}

void expr::parse(string_view str, std::vector<state>& states)
{
    std::queue<string> output;
    std::stack<op>     operators;

    auto handle_op = [&](op o)
    {
        while (!operators.empty() && operators.top() != op::paren_open && o <= operators.top())
        {
            output.emplace(op_to_char(operators.top()));
            operators.pop();
        }
        operators.push(o);
    };

    std::size_t first_char = 0;

    for (std::size_t i = 0; i < str.length(); ++i)
    {
        const auto c = str[i];

        switch (c)
        {
        case '(': operators.push(op::paren_open); break;
        case ')':
            if (operators.empty()) throw parse_exception("mismatched parentheses");

            while (operators.top() != op::paren_open)
            {
                output.emplace(op_to_char(operators.top()));
                operators.pop();

                if (operators.empty()) throw parse_exception("mismatched parentheses");
            }
            operators.pop();
            break;

        case '|': handle_op(op::vertical_bar); break;

        case '[': operators.push(op::bracket_open); break;
        case ']':
            if (operators.empty()) throw parse_exception("mismatched brackets");

            while (operators.top() != op::bracket_open)
            {
                output.emplace(op_to_char(operators.top()));
                operators.pop();

                if (operators.empty()) throw parse_exception("mismatched brackets");
            }
            operators.pop();
            break;

        case '.': handle_op(op::period); break;

        case '*': handle_op(op::asterisk); break;
        case '+': handle_op(op::plus); break;
        case '?': handle_op(op::question_mark); break;

        default:
            first_char = i;
            while (i < str.length() && !is_operator(str[i]))
            {
                ++i;
                if (str[i] == '\\') ++i;
            }

            string lit{str.substr(first_char, std::min(i, str.length()) - first_char)};
            lit = util::replace(static_cast<string_view>(lit), R"(\)"sv, ""sv);
            output.push(lit);
        }
    }

    while (!operators.empty())
    {
        auto op = operators.top();
        switch (op)
        {
        case op::paren_open: [[fallthrough]];
        case op::paren_close: throw parse_exception("mismatched parentheses");

        default: break;
        }

        output.emplace(op_to_char(op));
        operators.pop();
    }

    while (!output.empty())
    {
        auto next = output.front();
        output.pop();
    }
}

bool expr::match(string_view input) const noexcept
{
    auto        state_it = states.begin();
    std::size_t offset   = 0;

    while (offset < input.length() && state_it != states.end())
    {
        if (!state_it->match(input, offset)) return false;

        ++state_it;
    }

    // if all states matched, then it is a match.
    return state_it == states.end();
}

constexpr bool expr::should_repeat(repetition rep, std::size_t current_matches) noexcept
{
    switch (rep)
    {
    case repetition::once: return current_matches < 1;
    case repetition::zero_or_one: return current_matches < 1;
    case repetition::zero_or_more: return true;
    case repetition::one_or_more: return true;
    }
}

constexpr bool expr::repetition_satisfied(repetition rep, std::size_t current_matches) noexcept
{
    switch (rep)
    {
    case repetition::once: return current_matches == 1;
    case repetition::zero_or_one: return current_matches < 2;
    case repetition::zero_or_more: return true;
    case repetition::one_or_more: return current_matches > 0;
    }
}

bool expr::state_literal::match(string_view str, std::size_t& offset) const noexcept
{
    if (str.substr(offset).starts_with(literal))
    {
        offset += literal.length();
        return true;
    }

    return false;
}

bool expr::state_string_alternation::match(string_view str, std::size_t& offset) const noexcept
{
    for (const auto& choice : choices)
    {
        if (str.substr(offset).starts_with(choice))
        {
            offset += choice.length();
            return true;
        }
    }

    return false;
}

bool expr::state_char_alternation::match(string_view str, std::size_t& offset) const noexcept
{
    for (auto choice : choices)
    {
        if (str[offset] == choice)
        {
            ++offset;
            return true;
        }
    }

    return false;
}

bool expr::state_wildcard::match(string_view str, std::size_t& offset) const noexcept
{
    if (str[offset] == '.')
    {
        ++offset;
        return true;
    }

    return false;
}

bool expr::state::match(string_view str, std::size_t& offset) const noexcept
{
    std::size_t match_count = 0;
    bool        did_match   = false;

    // NOLINTNEXTLINE(cppcoreguidelines-avoid-do-while)
    do {
        did_match = std::visit([&](const auto& m) { return m.match(str, offset); }, impl);
        if (did_match) ++match_count;
    }
    while (did_match && should_repeat(rep, match_count));

    return repetition_satisfied(rep, match_count);
}
}
