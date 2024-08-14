#include "http/router.hpp"

#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

#include "coro/task.hpp"
#include "http/request.hpp"
#include "http/response.hpp"
#include "util/string_util.hpp"

namespace net::http
{

router& router::add(request_method method, std::string_view route, handler_func&& h)
{
    auto& sub                   = get_subrouter(route);
    sub.method_handlers[method] = h;

    return *this;
}

coro::task<void> router::operator()(const server_request& req, response_writer& resp) const
{
    std::string_view route = req.uri.path;

    if (route.starts_with('/'))
    {
        route = route.substr(1);
    }

    auto* parent = this;
    for (const auto part : util::split_string(route, '/'))
    {
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
                current->wildcard      = std::make_unique<router>();
                current->wildcard_name = part.substr(1);
            }
            else if (current->wildcard_name != part.substr(1))
            {
                throw std::runtime_error{"mismatched wildcard name"};
            }

            current = current->wildcard.get();
        }
        else
        {
            current = &current->children[part];
        }
    }

    return *current;
}

}
