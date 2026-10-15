#include "http/router.hpp"

#include <algorithm>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "coro/task.hpp"
#include "http/request.hpp"
#include "http/request_target.hpp"
#include "http/response.hpp"
#include "url.hpp"
#include "util/string_util.hpp"

namespace net::http
{

using namespace std::string_view_literals;

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

    const auto*                                      parent = this;
    std::vector<std::pair<std::string, std::string>> values;

    // The path is split before it's decoded, so an encoded '/' ("%2F") is part of a segment rather than separating two.
    // A literal segment is taken over a parameter, and never given up for it later: there's no backtracking.
    for (const auto encoded_part : util::split_string(route, '/'))
    {
        auto part = url::decode(encoded_part);

        auto iter = parent->children.find(part);
        if (iter != parent->children.end())
        {
            parent = &iter->second;
        }
        else if (parent->wildcard != nullptr && !part.empty())
        {
            values.emplace_back(parent->wildcard_name, std::move(part));
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

    req.route       = parent->pattern;
    req.path_values = std::move(values);

    auto iter = parent->method_handlers.find(req.method);

    // HEAD is GET without the body (which the server drops), unless the route has its own handler for it.
    if (iter == parent->method_handlers.end() && req.method == request_method::HEAD)
        iter = parent->method_handlers.find(request_method::GET);

    if (iter == parent->method_handlers.end())
    {
        resp.headers().set("Allow"sv, parent->allowed_methods());
        co_await resp.send(status::METHOD_NOT_ALLOWED, 0);
        co_return;
    }

    co_await std::invoke(iter->second, req, resp);
}

std::string router::allowed_methods() const
{
    std::vector<request_method> methods;
    methods.reserve(method_handlers.size() + 1);
    for (const auto& [method, _] : method_handlers) methods.push_back(method);
    if (method_handlers.contains(request_method::GET) && !method_handlers.contains(request_method::HEAD))
        methods.push_back(request_method::HEAD);

    std::ranges::sort(methods);

    std::string out;
    for (auto method : methods)
    {
        if (!out.empty()) out += ", ";
        out += method_string(method);
    }

    return out;
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
