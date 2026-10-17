#include "exception.hpp"

#include <cerrno>
#include <string>
#include <system_error>

#include <netdb.h>

namespace
{

struct gai_error_category : std::error_category
{
    [[nodiscard]] const char* name() const noexcept override { return "getaddrinfo"; }
    [[nodiscard]] std::string message(int err) const override { return ::gai_strerror(err); }
};

const gai_error_category the_gai_category;

}

namespace net
{

const std::error_category& gai_category() noexcept { return the_gai_category; }

std::error_code make_gai_error_code(int err) noexcept
{
    if (err == EAI_SYSTEM) return {errno, std::system_category()};
    return {err, the_gai_category};
}

[[nodiscard]] std::system_error system_error_from_errno(int err, const std::string& what) noexcept
{
    const std::error_code code{err, std::system_category()};
    return std::system_error{code, what};
}

bool throw_for_gai_error(int err)
{
    switch (err)
    {
    case 0: return false;
    case EAI_AGAIN: return true;

    case EAI_ADDRFAMILY: throw exception{"address family for hostname not supported"};
    case EAI_BADFLAGS: throw exception{"internal error: bad ai_flags value"};
    case EAI_FAIL: throw exception{"non-recoverable failure in name resolution"};
    case EAI_FAMILY: throw exception{"selected protocol is not supported"};
    case EAI_MEMORY: throw exception{"memory allocation failure"};
    case EAI_NODATA: throw exception{"no address associated with hostname"};
    case EAI_NONAME: throw exception{"host or port not found"};
    case EAI_SERVICE: throw exception{"unsupported port for selected network type"};
    case EAI_SOCKTYPE: throw exception{"selected network is not supported"};
    case EAI_SYSTEM: throw system_error_from_errno(errno);
#ifdef EAI_BADHINTS
    case EAI_BADHINTS: throw exception{"internal error: bad hints value"};
#endif
#ifdef EAI_PROTOCOL
    case EAI_PROTOCOL: throw exception{"resolved protocol is unknown"};
#endif
    case EAI_OVERFLOW: throw exception{"internal error: argument buffer overflow"};
    default: throw exception{"EAI error code: " + std::to_string(err)};
    }
}

}
