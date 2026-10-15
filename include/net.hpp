#pragma once

#include <expected>
#include <string>
#include <system_error>

namespace net
{

// hostname returns this machine's host name.
std::expected<std::string, std::error_condition> hostname();

}
