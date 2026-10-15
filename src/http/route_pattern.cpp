#include "http/route_pattern.hpp"

#include <string>
#include <system_error>

namespace
{

struct route_error_category : std::error_category
{
    [[nodiscard]] const char* name() const noexcept override { return "net::http::route_error"; }

    [[nodiscard]] std::string message(int ev) const override
    {
        using enum net::http::route_error;
        switch (static_cast<net::http::route_error>(ev))
        {
        case none: return "no error";
        case no_leading_slash: return "a route pattern must start with '/'";
        case query_or_fragment: return "a route pattern can't have '?' or '#'";
        case empty_parameter_name: return "a route parameter needs a name";
        case invalid_parameter_name:
            return "a route parameter's name is letters, digits and '_', not starting with a digit";
        case duplicate_parameter_name: return "a route parameter's name is used twice";
        default: return "(unrecognized)";
        }
    }
};

const route_error_category the_route_error_category;

}

namespace net::http
{

std::error_condition make_error_condition(route_error err) noexcept
{
    return {static_cast<int>(err), the_route_error_category};
}

const std::error_category& route_error_category() noexcept { return the_route_error_category; }

}
