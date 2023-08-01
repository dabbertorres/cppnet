#pragma once

#include <cstddef>
#include <system_error>

namespace net::io
{

struct result
{
    std::size_t          count{};
    std::error_condition err{};

    // is_eof returns true if the result indicates an end-of-file condition.
    // This is defined by a short read, but with no error condition.
    // read_request should be the amount passed to a io::reader::read().
    [[nodiscard]] bool is_eof(std::size_t read_request) const noexcept { return read_request < count && !err; }
};

}
