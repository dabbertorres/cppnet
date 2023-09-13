#include "exception.hpp"

#include <string>

#include <netdb.h>

namespace net
{

[[nodiscard]] std::system_error system_error_from_errno(int err, const char* what) noexcept
{
    auto code = std::make_error_code(static_cast<std::errc>(err));
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
    case EAI_NONAME: throw exception{"at least one of host and port must be specified"};
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
