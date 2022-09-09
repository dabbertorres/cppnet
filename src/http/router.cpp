#include "http/router.hpp"

namespace net::http
{

router& router::add(route&& route) noexcept
{
    routes.emplace_back(std::move(route));
    return *this;
}

void router::operator()(const request& req, response& resp) const
{
    for (const auto& r : routes)
    {
        if (r.matcher(req))
        {
            r.handler(req, resp);
            return;
        }
    }
}

}
