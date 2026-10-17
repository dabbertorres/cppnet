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

}
