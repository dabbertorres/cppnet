#pragma once

#include <cerrno>
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

// errno_code is the error code of an OS error (errno's value, by default, which it reads before anything else can
// change it: call it right after the call that failed), in std::system_category().
[[nodiscard]] inline std::error_code errno_code(int err = errno) noexcept { return {err, std::system_category()}; }

[[nodiscard]] std::system_error system_error_from_errno(int err, const std::string& what = "") noexcept;

// gai_category is the category of getaddrinfo()'s EAI_* errors.
[[nodiscard]] const std::error_category& gai_category() noexcept;

// make_gai_error_code converts an error from getaddrinfo(). EAI_SYSTEM is reported as errno.
[[nodiscard]] std::error_code make_gai_error_code(int err) noexcept;

}
