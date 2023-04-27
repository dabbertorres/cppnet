#include "http/router.hpp"

#include <algorithm>
#include <ranges>

#include "util/iterator.hpp"

namespace net::http
{

route& route::prefix(const std::string& prefix)
{
    conditions.emplace_back([=](const server_request& req) { return req.uri.path.starts_with(prefix); });
    return *this;
}

route& route::path(const std::string& path)
{
    conditions.emplace_back([=](const server_request& req) { return req.uri.path == path; });
    return *this;
}

route& route::on_method(request_method method)
{
    conditions.emplace_back([=](const server_request& req) { return req.method == method; });
    return *this;
}

route& route::on_content_type(const std::string& want_content_type)
{
    conditions.emplace_back(
        [=](const server_request& req)
        {
            auto content = req.headers.get_content_type();
            return content.has_value() && content->type == want_content_type;
        });
    return *this;
}

[[nodiscard]] bool route::matches(const server_request& req) const
{
    return std::ranges::all_of(conditions, [&req](const auto& cond) { return std::invoke(cond, req); });
}

void   router::add(const route& r) { routes.push_back(r); }
route& router::add() { return routes.emplace_back(); }
route& router::prefix(const std::string& prefix) { return add().prefix(prefix); }
route& router::path(const std::string& path) { return add().path(path); }

[[nodiscard]] std::optional<std::reference_wrapper<const handler_func>>
router::route_request(const server_request& req) const
{
    auto route = std::ranges::find_if(routes, [&](const auto& route) { return route.matches(req); });
    return util::optional_from_iterator(routes, route).transform([](const auto& route) { return route.handler; });
}

}
