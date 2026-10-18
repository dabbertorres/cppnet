#pragma once

#include <stdexcept>
#include <string>
#include <system_error>

namespace net
{

class exception : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

[[nodiscard]] std::system_error system_error_from_errno(int err, const std::string& what = "") noexcept;

// gai_category is the category of getaddrinfo()'s EAI_* errors.
[[nodiscard]] const std::error_category& gai_category() noexcept;

// make_gai_error_code converts an error from getaddrinfo(). EAI_SYSTEM is reported as errno.
[[nodiscard]] std::error_code make_gai_error_code(int err) noexcept;

}
