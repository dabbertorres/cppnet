#include "http/router.hpp"

namespace net::http
{

router& router::route(method m, std::string_view path, std::unique_ptr<handler> h) noexcept
{
    auto match = std::make_unique<method_path_matcher>(m, path);
    return route(std::move(match), std::move(h));
}

router& router::route(std::unique_ptr<matcher> m, std::unique_ptr<handler> h) noexcept
{
    routes.emplace_back(std::move(m), std::move(h));
    return *this;
}

void router::handle(const request& req, response& resp)
{
    for (auto& r : routes)
    {
        if (r.matcher->match(req))
        {
            r.handler->handle(req, resp);
            return;
        }
    }
}

router::route_matcher::route_matcher(std::unique_ptr<::net::http::matcher>&& m,
                                     std::unique_ptr<::net::http::handler>&& h) noexcept :
    matcher{std::move(m)},
    handler{std::move(h)}
{}

router::route_matcher::route_matcher(route_matcher&& other) noexcept :
    matcher{std::move(other.matcher)},
    handler{std::move(other.handler)}
{}

router::route_matcher& router::route_matcher::operator=(route_matcher&& other) noexcept
{
    matcher = std::move(other.matcher);
    handler = std::move(other.handler);
    return *this;
}

}
