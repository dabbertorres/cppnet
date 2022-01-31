#include "net.hpp"

#include <unistd.h>

namespace net
{

std::string hostname() noexcept
{
    static size_t len = sysconf(_SC_HOST_NAME_MAX) + 1;

    std::string name(len, '\0');
    gethostname(name.data(), name.size());

    auto end = name.find_first_of('\0');
    if (end != std::string::npos)
    {
        name.resize(end);
        name.shrink_to_fit();
    }
    return name;
}

}
