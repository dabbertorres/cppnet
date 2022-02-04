#pragma once

#include <chrono>
#include <streambuf>
#include <string>
#include <string_view>
#include <vector>

#include "exception.hpp"
#include "reader.hpp"
#include "writer.hpp"

namespace net
{

enum class addr_protocol
{
    not_care,
    ipv4,
    ipv6,
};

class socket : public std::basic_streambuf<uint8_t>, public reader, public writer
{
public:
    using streambuf = std::basic_streambuf<uint8_t>;

    socket();
    socket(size_t buf_size);
    socket(int fd, size_t buf_size = 256);

    // non-copyable
    socket(const socket&) = delete;
    socket& operator=(const socket&) = delete;

    // movable
    socket(socket&& other) noexcept;
    socket& operator=(socket&& other) noexcept;

    virtual ~socket();

    bool        valid() const noexcept;
    std::string local_addr() const;
    std::string remote_addr() const;

    int underflow() override;
    int overflow(int ch) override;
    int sync() override;

    void flush() noexcept override { sync(); }

protected:
    static constexpr int invalid_fd = -1;

    int fd;

private:
    std::vector<streambuf::char_type> read_buf;
    std::vector<streambuf::char_type> write_buf;
};

}
