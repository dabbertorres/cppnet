#include "net.hpp"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <expected>
#include <string>
#include <system_error>

#include <unistd.h>

namespace net
{

std::expected<std::string, std::error_condition> hostname()
{
    // the limit, if there is one, doesn't include the terminating null.
    const auto max = sysconf(_SC_HOST_NAME_MAX);
    const auto len = static_cast<std::size_t>(max > 0 ? max : _POSIX_HOST_NAME_MAX) + 1;

    std::string name(len, '\0');
    if (gethostname(name.data(), name.size()) != 0)
    {
        return std::unexpected(std::error_condition{errno, std::generic_category()});
    }

    // it may be truncated without being terminated, if it's too long for the buffer.
    name.resize(std::min(name.find('\0'), name.size()));
    return name;
}

}
