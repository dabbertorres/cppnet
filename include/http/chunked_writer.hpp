#include <cstddef>

#include "io/io.hpp"
#include "io/writer.hpp"

namespace net::http::http11
{

class chunked_writer : public io::writer
{
public:
    constexpr chunked_writer(io::writer* writer)
        : parent{writer}
    {}

    io::result write(const std::byte* data, std::size_t length) override;

    using io::writer::write;

    [[nodiscard]] int native_handle() const noexcept override { return parent->native_handle(); }

private:
    io::writer* parent = nullptr;
};

}
