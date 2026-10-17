#pragma once

#include <expected>
#include <string_view>
#include <system_error>
#include <vector>

#include "ip_addr.hpp"

namespace net
{

// dns_lookup resolves hostname's addresses with the system's resolver (getaddrinfo()), failing with its error (in
// gai_category()). It blocks until it's done: from a coroutine, call it with io::scheduler::run_blocking.
std::expected<std::vector<ip_addr>, std::error_code> dns_lookup(std::string_view hostname);
}
