#pragma once

#include <stdexcept>
#include <system_error>

namespace net
{

class exception : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

[[nodiscard]] std::system_error system_error_from_errno(int err, const char* what = nullptr) noexcept;

// throw_for_gai_error throws an exception if err is unrecoverable (or logical) error.
// If err is recoverable (e.g. retry), then it returns true.
// Otherwise, err is indicates success, and false is returned.
bool throw_for_gai_error(int err);

}
