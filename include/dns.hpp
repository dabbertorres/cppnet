#pragma once

#include <string_view>

#include "coro/generator.hpp"

#include "ip_addr.hpp"

namespace net
{

coro::generator<ip_addr> dns_lookup(std::string_view hostname);

}
