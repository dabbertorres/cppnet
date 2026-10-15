#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string_view>
#include <system_error>
#include <type_traits>

namespace net::http
{

// route_error is what's wrong with a route pattern (see route_pattern).
enum class route_error : std::uint8_t
{
    none,
    no_leading_slash,         // a pattern is a path: e.g. "/users", not "users".
    query_or_fragment,        // '?' or '#', which a path can't have, so the route would never match.
    empty_parameter_name,     // e.g. "/users/:".
    invalid_parameter_name,   // a name is letters, digits and '_', not starting with a digit.
    duplicate_parameter_name, // e.g. "/a/:id/b/:id", which would make path_value ambiguous.
};

std::error_condition make_error_condition(route_error err) noexcept;

const std::error_category& route_error_category() noexcept;

namespace detail
{

    // is_parameter_name is whether name is letters, digits and '_', not starting with a digit.
    constexpr bool is_parameter_name(std::string_view name) noexcept
    {
        for (std::size_t i = 0; i < name.size(); ++i)
        {
            const char c       = name[i];
            const bool letter  = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
            const bool digit   = c >= '0' && c <= '9';
            const bool allowed = letter || (digit && i > 0);
            if (!allowed) return false;
        }

        return true;
    }

    // has_segment is whether path (which ends with '/') has segment as a whole segment.
    constexpr bool has_segment(std::string_view path, std::string_view segment) noexcept
    {
        for (auto pos = path.find(segment); pos != std::string_view::npos; pos = path.find(segment, pos + 1))
        {
            const auto after = pos + segment.size();
            if (pos > 0 && path[pos - 1] == '/' && after < path.size() && path[after] == '/') return true;
        }

        return false;
    }

}

// check_route_pattern checks a route pattern on its own. (Whether it conflicts with other routes, by naming a
// parameter differently, is checked as it's added to a router.)
// NOLINTNEXTLINE(bugprone-exception-escape): substr's positions are in range.
constexpr route_error check_route_pattern(std::string_view pattern) noexcept
{
    if (!pattern.starts_with('/')) return route_error::no_leading_slash;
    if (pattern.find_first_of("?#") != std::string_view::npos) return route_error::query_or_fragment;

    for (std::size_t start = 1; start <= pattern.size();)
    {
        auto end = pattern.find('/', start);
        if (end == std::string_view::npos) end = pattern.size();

        const auto segment = pattern.substr(start, end - start);
        const auto before  = pattern.substr(0, start); // up to the '/' before segment
        start              = end + 1;

        if (!segment.starts_with(':')) continue;

        const auto name = segment.substr(1);
        if (name.empty()) return route_error::empty_parameter_name;
        if (!detail::is_parameter_name(name)) return route_error::invalid_parameter_name;
        if (detail::has_segment(before, segment)) return route_error::duplicate_parameter_name;
    }

    return route_error::none;
}

// route_pattern is a router's route (see router): e.g. "/users/:id". It's checked as it's made, so a bad pattern given
// as a string literal fails to compile, with the reason at the line that fails here. A pattern only known at runtime
// is made by parse instead.
//
// It views its string, which must outlive it (as a string literal does).
class route_pattern
{
public:
    // NOLINTNEXTLINE(*-explicit-constructor): string literals convert, being checked at compile time.
    consteval route_pattern(const char* str)
        : route_pattern{std::string_view{str}}
    {}

    // NOLINTNEXTLINE(*-explicit-constructor): string literals convert, being checked at compile time.
    consteval route_pattern(std::string_view str)
        : value{str}
    {
        // A throw isn't a constant expression, so reaching one fails to compile, pointing here.
        // NOLINTBEGIN(bugprone-std-exception-baseclass): never thrown: it fails compilation instead.
        switch (check_route_pattern(str))
        {
        case route_error::none: break;
        case route_error::no_leading_slash: throw "a route pattern must start with '/'";
        case route_error::query_or_fragment: throw "a route pattern can't have '?' or '#'";
        case route_error::empty_parameter_name: throw "a route parameter needs a name";
        case route_error::invalid_parameter_name:
            throw "a route parameter's name is letters, digits and '_', not starting with a digit";
        case route_error::duplicate_parameter_name: throw "a route parameter's name is used twice";
        }
        // NOLINTEND(bugprone-std-exception-baseclass)
    }

    // parse checks a pattern that's only known at runtime.
    static std::expected<route_pattern, std::error_condition> parse(std::string_view str) noexcept
    {
        if (auto err = check_route_pattern(str); err != route_error::none)
            return std::unexpected(make_error_condition(err));
        return route_pattern{checked{}, str};
    }

    [[nodiscard]] constexpr std::string_view view() const noexcept { return value; }

private:
    struct checked
    {};

    constexpr route_pattern(checked /*unused*/, std::string_view str) noexcept
        : value{str}
    {}

    std::string_view value;
};

}

namespace std
{

template<>
struct is_error_condition_enum<net::http::route_error> : std::true_type
{};

}
