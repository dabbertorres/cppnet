#include "http/router.hpp"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "coro/task.hpp"
#include "http/headers.hpp"
#include "http/request.hpp"
#include "http/request_target.hpp"
#include "http/response.hpp"
#include "url.hpp"
#include "util/small_vector.hpp"
#include "util/string_util.hpp"

namespace net::http
{

using namespace std::string_view_literals;

router& router::add(request_method method, route_pattern route, handler_func&& h)
{
    auto& sub                   = get_subrouter(route.view());
    sub.method_handlers[method] = std::make_unique<route_handler>(std::move(h), sub.layers);

    return *this;
}

router::route_handler::route_handler(handler_func&& h, const std::vector<layer_ptr>& layers)
    : handler{std::move(h)}
{
    set_layers(layers);
}

void router::route_handler::set_layers(const std::vector<layer_ptr>& from)
{
    layers = from;

    steps.clear();
    nexts.clear();
    steps.reserve(layers.size() + 1);
    nexts.reserve(layers.size() + 1);

    // NOTE: the steps are all made before the next_handlers that refer to them.
    for (std::size_t i = 0; i <= layers.size(); ++i) steps.push_back({.owner = this, .index = i});
    for (const auto& s : steps) nexts.emplace_back(s);
}

coro::task<void> router::route_handler::step::operator()(server_request& req, response_writer& resp) const
{
    if (index == owner->layers.size()) return std::invoke(owner->handler, req, resp);

    // NOTE: the layer's coroutine may hold the next_handler it's given by reference: it's the route's own.
    return std::invoke(*owner->layers[index], req, resp, owner->nexts[index + 1]);
}

void router::add_layer(const layer_ptr& l) { insert_layer(layers.size(), l); }

void router::insert_layer(std::size_t index, const layer_ptr& l)
{
    // everything under this router has its layers first (as they were made from them), so index is the same place in
    // each: after this router's, before its subrouters' own.
    layers.insert(layers.begin() + static_cast<std::ptrdiff_t>(index), l);

    for (auto& [_, h] : method_handlers) h->set_layers(layers);
    for (auto& [_, child] : children) child.insert_layer(index, l);
    if (wildcard != nullptr) wildcard->insert_layer(index, l);
}

coro::task<void> router::operator()(server_request& req, response_writer& resp) const
{
    std::string_view route = req.target.path;

    if (route.starts_with('/'))
    {
        route = route.substr(1);
    }

    const auto* parent = this;

    // the parameters are added as they're found, and removed if the path leads nowhere.
    const auto not_found = [&req]
    {
        req.path_values.clear();
        req.path_value_storage.clear();
    };

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
            req.add_path_value(parent->wildcard_name, encoded_part);
            parent = parent->wildcard.get();
        }
        else
        {
            not_found();
            co_await resp.send(status::NOT_FOUND, 0);
            co_return;
        }
    }

    // a router with no handlers is only part of the way to a route.
    if (parent->method_handlers.empty())
    {
        not_found();
        co_await resp.send(status::NOT_FOUND, 0);
        co_return;
    }

    req.route = parent->pattern;

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

    co_await (*iter->second)(req, resp);
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
    // NOTE: route_pattern has checked the route on its own: it starts with '/', and its parameters have names.
    route = route.substr(1);

    auto* current = this;
    for (const auto part : util::split_string(route, '/'))
    {
        if (part.starts_with(':'))
        {
            if (current->wildcard == nullptr)
            {
                current->wildcard          = std::make_unique<router>();
                current->wildcard_name     = part.substr(1);
                current->wildcard->pattern = current->pattern + '/' + std::string{part};
                current->wildcard->layers  = current->layers;
            }
            else if (current->wildcard_name != part.substr(1))
            {
                // the router keeps one parameter per position, which path_value finds by its name.
                throw std::invalid_argument{"route parameter " + std::string{part} + " conflicts with :"
                                            + current->wildcard_name + ", which another route has in its place"};
            }

            current = current->wildcard.get();
        }
        else
        {
            auto [iter, added] = current->children.try_emplace(std::string{part});
            if (added)
            {
                iter->second.pattern = current->pattern + '/' + std::string{part};
                iter->second.layers  = current->layers;
            }

            current = &iter->second;
        }
    }

    return *current;
}

}
