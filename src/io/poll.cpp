#include "io/poll.hpp"

#include <type_traits>

namespace net::io
{

poll_op operator&(poll_op lhs, poll_op rhs) noexcept
{
    auto lhs_v = static_cast<std::underlying_type_t<poll_op>>(lhs);
    auto rhs_v = static_cast<std::underlying_type_t<poll_op>>(rhs);

    auto v = lhs_v & rhs_v;
    return static_cast<poll_op>(v);
}

poll_op operator|(poll_op lhs, poll_op rhs) noexcept
{
    auto lhs_v = static_cast<std::underlying_type_t<poll_op>>(lhs);
    auto rhs_v = static_cast<std::underlying_type_t<poll_op>>(rhs);

    auto v = lhs_v | rhs_v;
    return static_cast<poll_op>(v);
}

bool readable(poll_op op) noexcept { return (op & poll_op::read) == poll_op::read; }
bool writable(poll_op op) noexcept { return (op & poll_op::write) == poll_op::write; }

}
