#include <cstddef>

#include "io/io.hpp"
#include "io/reader.hpp"

namespace net::http::http11
{

class chunked_reader : public io::reader
{
public:
    constexpr chunked_reader(io::reader* reader)
        : parent{reader}
    {}

    io::result read(std::byte* data, std::size_t length) override;

    using io::reader::read;

    [[nodiscard]] int native_handle() const noexcept override { return parent->native_handle(); }

private:
    io::result get_next_chunk_size();
    io::result validate_end_of_chunk();

    reader*     parent             = nullptr;
    std::size_t current_chunk_size = 0;
};

}
