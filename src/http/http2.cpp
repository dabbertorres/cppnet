//#include "http2.hpp"

#include <cstdint>
#include <string>

#include "packed.hpp"

namespace net::http2
{

enum class frame_type : uint8_t
{
    DATA          = 0x0,
    HEADERS       = 0x1,
    PRIORITY      = 0x2,
    RST_STREAM    = 0x3,
    SETTINGS      = 0x4,
    PUSH_PROMISE  = 0x5,
    PING          = 0x6,
    GOAWAY        = 0x7,
    WINDOW_UPDATE = 0x8,
    CONTINUATION  = 0x9,
};

enum class frame_flags : uint8_t
{
    END_STREAM  = 0x01,
    ACK         = 0x01,
    END_HEADERS = 0x04,
    PADDED      = 0x08,
    PRIORITY    = 0x20,
};

struct NET_PACKED frame_header
{
    uint32_t    length : 24;
    frame_type  type;
    frame_flags flags;
    uint32_t    R                 : 1;
    uint32_t    stream_identifier : 31;
};

static_assert(sizeof(frame_header) == 9);

}
