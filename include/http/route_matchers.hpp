#pragma once

#include "http/router.hpp"

namespace net::http::matchers
{

struct method_path : public matcher
{
    method_path(method m, std::string_view p)
        : method{m}
        , path{p}
    {}

    const method      method;
    const std::string path;

    [[nodiscard]] bool operator()(const request& req) const noexcept final
    {
        return req.uri.path == path && method == req.method;
    }
};

}
