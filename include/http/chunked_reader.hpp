#include <cstddef>
#include <memory>
#include <span>
#include <utility>

#include "io/io.hpp"
#include "io/reader.hpp"

namespace net::http::http11
{

class chunked_reader : public io::reader
{
public:
    constexpr chunked_reader(std::unique_ptr<io::reader>&& reader)
        : parent{std::move(reader)}
    {}

    io::result read(std::span<std::byte> data) override;

    using io::reader::read;

    [[nodiscard]] int native_handle() const noexcept override { return parent->native_handle(); }

private:
    io::result get_next_chunk_size();
    io::result validate_end_of_chunk();

    std::unique_ptr<io::reader> parent             = nullptr;
    std::size_t                 current_chunk_size = 0;
};

}
