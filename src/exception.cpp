#include "exception.hpp"

#include <cerrno>
#include <cstring>

#include <netdb.h>

namespace net
{

exception::exception() : exception(::strerror(errno)) {}

exception::exception(int status) : exception(::gai_strerror(status)) {}

}
