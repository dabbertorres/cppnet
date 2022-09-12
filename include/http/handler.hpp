#pragma once

#include <concepts>

namespace net::http
{

struct request;
struct server_response;

template<typename T>
concept Handler = std::invocable<T, const request&, server_response&>;

}
