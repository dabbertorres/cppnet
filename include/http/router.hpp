#pragma once

#include <concepts>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "http/request.hpp"
#include "http/response.hpp"

namespace net::http
{

template<typename T>
concept Matcher =
    std::invocable<T, const server_request&> && std::same_as<std::invoke_result_t<T, const server_request&>, bool>;

template<typename T>
concept Handler = std::invocable<T, const server_request&, response_writer&>;

using matcher_func = std::function<bool(const server_request&)>;
using handler_func = std::function<void(const server_request&, response_writer&)>;

class router;

struct route
{
public:
    route() = default;

    route& prefix(const std::string& prefix);
    route& path(const std::string& path);
    route& method(request_method method);
    route& content_type(const std::string& content_type);

    template<Matcher M>
    route& on(M&& m)
    {
        conditions.push_back(std::forward<M>(m));
        return *this;
    }

    template<Handler H>
    route& use(H&& h)
    {
        handler = std::forward<H>(h);
        return *this;
    }

    [[nodiscard]] bool matches(const server_request& req) const;

private:
    friend class router;

    std::vector<matcher_func> conditions;
    handler_func              handler;
};

class router final
{
public:
    void   add(const route& r);
    route& add();
    route& prefix(const std::string& prefix);
    route& path(const std::string& path);
    route& method(request_method method);

    [[nodiscard]] std::optional<std::reference_wrapper<const handler_func>>
    route_request(const server_request& req) const;

    void operator()(const server_request&, response_writer&) const;

private:
    std::vector<route> routes;
};

}
