#include "http/router.hpp"

#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "coro/task.hpp"
#include "http/request.hpp"
#include "http/response.hpp"
#include "url.hpp"
#include "util/string_util.hpp"

namespace net::http
{

router& router::add(request_method method, std::string_view route, handler_func&& h)
{
    auto& sub                   = get_subrouter(route);
    sub.method_handlers[method] = std::move(h);

    return *this;
}

coro::task<void> router::operator()(server_request& req, response_writer& resp) const
{
    std::string_view route = req.target.path;

    if (route.starts_with('/'))
    {
        route = route.substr(1);
    }

    const auto* parent = this;

    // The path is split before it's decoded, so an encoded '/' ("%2F") is part of a segment rather than separating two.
    for (const auto encoded_part : util::split_string(route, '/'))
    {
        const auto part = url::decode(encoded_part);

        auto iter = parent->children.find(part);
        if (iter != parent->children.end())
        {
            parent = &iter->second;
        }
        else if (parent->wildcard != nullptr)
        {
            parent = parent->wildcard.get();
        }
        else
        {
            co_await resp.send(status::NOT_FOUND, 0);
            co_return;
        }
    }

    // a router with no handlers is only part of the way to a route.
    if (parent->method_handlers.empty())
    {
        co_await resp.send(status::NOT_FOUND, 0);
        co_return;
    }

    req.route = parent->pattern;

    auto iter = parent->method_handlers.find(req.method);
    if (iter == parent->method_handlers.end())
    {
        co_await resp.send(status::METHOD_NOT_ALLOWED, 0);
        co_return;
    }

    co_await std::invoke(iter->second, req, resp);
}

router& router::get_subrouter(std::string_view route)
{
    if (route.starts_with('/'))
    {
        route = route.substr(1);
    }

    auto* current = this;
    for (const auto part : util::split_string(route, '/'))
    {
        // wildcard
        if (part.starts_with(':'))
        {
            if (part.length() == 1)
            {
                throw std::runtime_error{"wildcard name must not be empty"};
            }

            if (current->wildcard == nullptr)
            {
                current->wildcard          = std::make_unique<router>();
                current->wildcard_name     = part.substr(1);
                current->wildcard->pattern = current->pattern + '/' + std::string{part};
            }
            else if (current->wildcard_name != part.substr(1))
            {
                throw std::runtime_error{"mismatched wildcard name"};
            }

            current = current->wildcard.get();
        }
        else
        {
            auto [iter, added] = current->children.try_emplace(std::string{part});
            if (added) iter->second.pattern = current->pattern + '/' + std::string{part};

            current = &iter->second;
        }
    }

    return *current;
}

}
