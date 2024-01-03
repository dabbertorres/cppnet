#include "util/buffer_pool.hpp"

namespace net::util
{

buffer_pool::buffer_pool(std::size_t buffer_size, std::size_t num_buffers, std::size_t max_buffers)
    : buffers{num_buffers, max_buffers, [=] { return std::make_unique<buffer_t>(buffer_size); }}
{}

}
