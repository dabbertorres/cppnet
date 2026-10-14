#include <cstddef>
#include <memory>
#include <span>
#include <utility>

#include "coro/task.hpp"
#include "io/io.hpp"
#include "io/reader.hpp"

namespace net::http::http11
{

class chunked_reader : public io::reader
{
public:
    constexpr chunked_reader(std::unique_ptr<io::reader>&& reader)
        : owned{std::move(reader)}
        , parent{owned.get()}
    {}

    // reader is borrowed, and must outlive this chunked_reader.
    constexpr chunked_reader(io::reader* reader)
        : parent{reader}
    {}

    coro::task<io::result> read(std::span<std::byte> data) override;

    using io::reader::read;

    [[nodiscard]] int native_handle() const noexcept override { return parent->native_handle(); }

private:
    coro::task<io::result> get_next_chunk_size();
    coro::task<io::result> validate_end_of_chunk();

    std::unique_ptr<io::reader> owned              = nullptr;
    io::reader*                 parent             = nullptr;
    std::size_t                 current_chunk_size = 0;
    bool                        done               = false; // the final chunk has been read
};

}
