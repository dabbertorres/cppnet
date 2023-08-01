#include "net.hpp"

#include <climits>

#include <unistd.h>

namespace net
{

std::string hostname() noexcept
{
    auto len = static_cast<std::size_t>(sysconf(_SC_HOST_NAME_MAX) + 1);
    if (len < 0) len = _POSIX_HOST_NAME_MAX;

    std::string name(len, '\0');
    auto        err = gethostname(name.data(), name.size());
    if (err != 0)
        ; // TODO

    auto end = name.find_first_of('\0');
    if (end != std::string::npos)
    {
        name.resize(end);
        name.shrink_to_fit();
    }
    return name;
}

}
