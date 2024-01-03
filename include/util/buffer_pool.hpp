#pragma once

#include <array>
#include <cstddef>
#include <forward_list>

#include "io/reader.hpp"

#include "resource_pool.hpp"

namespace net::util
{

class buffer_pool
{
public:
    using buffer_t = std::vector<std::byte>;
    using pool_t = resource_pool<buffer_t>;

    buffer_pool(std::size_t buffer_size = 16ull * 1024, std::size_t num_buffers = 128, std::size_t max_buffers = 256);

private:
    pool_t buffers;
};

// TODO: rename to just "buffer" and then also implement io::writer?
class buffer_pool_reader : public io::reader
{
public:
    // TODO: read/write to *current_buf until full, then move to next item in buffers.
    // If at the end of buffers, borrow another buffer from *pool.

private:
    using buffer_list = std::forward_list<buffer_pool::pool_t::borrowed_resource>;

    buffer_pool* pool;
    buffer_list buffers;
    buffer_list::iterator current_buf;
};

}
