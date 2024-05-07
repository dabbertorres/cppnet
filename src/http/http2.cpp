#include "http/http2.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <system_error>
#include <unordered_map>

#include "http/request.hpp"
#include "http/response.hpp"
#include "io/buffered_reader.hpp"
#include "io/writer.hpp"
#include "util/packed.hpp"
#include "util/result.hpp"

namespace net::http::http2
{

using net::http::client_request;
using net::http::client_response;
using net::http::server_request;
using net::http::server_response;

// https://httpwg.org/specs/rfc9113.html#ErrorCodes
// Unknown or unsupported error codes MUST NOT trigger any special behavior.
// These MAY be treated by an implementation as being equivalent to INTERNAL_ERROR.
enum class error_code : std::uint32_t
{
    NO_ERROR            = 0x00,
    PROTOCOL_ERROR      = 0x01,
    INTERNAL_ERROR      = 0x02,
    FLOW_CONTROL_ERROR  = 0x03,
    SETTINGS_TIMEOUT    = 0x04,
    STREAM_CLOSED       = 0x05,
    FRAME_SIZE_ERROR    = 0x06,
    REFUSED_STREAM      = 0x07,
    CANCEL              = 0x08,
    COMPRESSION_ERROR   = 0x09,
    CONNECT_ERROR       = 0x0a,
    ENHANCE_YOUR_CALM   = 0x0b,
    INADEQUATE_SECURITY = 0x0c,
    HTTP_1_1_REQUIRED   = 0x0d,
};

enum class frame_type : std::uint8_t
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

enum class frame_flags : std::uint8_t
{
    END_STREAM  = 0x01,
    ACK         = 0x01,
    END_HEADERS = 0x04,
    PADDED      = 0x08,
    PRIORITY    = 0x20,
};

struct NET_PACKED frame_header
{
    std::uint32_t length : 24;
    frame_type    type;
    frame_flags   flags;
    std::uint32_t _                 : 1;
    std::uint32_t stream_identifier : 31;
};

static_assert(sizeof(frame_header) == 9);

struct NET_PACKED data_frame
{
    std::uint8_t pad_length; // only present if PADDED is set.
    // data...
    // ...padding;
};

struct NET_PACKED headers_frame
{
    std::uint8_t  pad_length;             // only present if PADDED is set.
    std::uint32_t exclusive         : 1;  // only present if PRIORITY is set.
    std::uint32_t stream_dependency : 31; // only present if PRIORITY is set.
    std::uint8_t  weight;                 // only present if PRIORITY is set.
    // field_block_fragment fragment;
    // padding...
};

// NOTE: priority_frame is deprecated.
struct NET_PACKED priority_frame
{
    std::uint32_t exclusive         : 1;
    std::uint32_t stream_dependency : 31;
    std::uint8_t  weight;
};

struct NET_PACKED rst_stream_frame
{
    error_code error_code;
};

// From: https://httpwg.org/specs/rfc9113.html#SettingValues
// NOTE: unknown ids must be ignored
enum class setting_id : std::uint16_t
{
    SETTINGS_HEADER_TABLE_SIZE      = 0x01,
    SETTINGS_ENABLE_PUSH            = 0x02,
    SETTINGS_MAX_CONCURRENT_STREAMS = 0x03,
    SETTINGS_INITIAL_WINDOW_SIZE    = 0x04,
    SETTINGS_MAX_FRAME_SIZE         = 0x05,
    SETTINGS_MAX_HEADER_LIST_SIZE   = 0x06,
};

struct NET_PACKED setting
{
    setting_id    identifier;
    std::uint32_t value;
};

struct NET_PACKED settings_frame
{
    std::span<setting> settings;
};

struct NET_PACKED push_promise_frame
{
    std::uint8_t  pad_length;             // only present if PADDED is set.
    std::uint32_t _                 : 1;  // RESERVED
    std::uint32_t stream_dependency : 31; // only present if PRIORITY is set.
    // field_block_fragment_id fragment_id;
    // padding...
};

struct NET_PACKED ping_frame
{
    std::array<std::byte, 8> opaque_data;
};

struct NET_PACKED goaway_frame
{
    std::uint32_t _              : 1;
    std::uint32_t last_stream_id : 31;
    error_code    error_code;
    // additional debug data
};

struct NET_PACKED window_update_frame
{
    std::uint32_t _                     : 1;
    std::uint32_t window_size_increment : 31;
};

struct NET_PACKED continuation_frame
{
    // field_block_fragment fragment;
};

// TODO: psuedo-headers

// https://httpwg.org/specs/rfc9113.html#StreamStates
// idle:
//   recv PUSH_PROMISE -> reserved (remote)
//   send PUSH_PROMISE -> reserved (local)
//   send HEADERS -> open
//   recv HEADERS -> open
//
// reserved (remote):
//   recv HEADERS -> half-closed (local)
//   send RST_STREAM -> closed
//   recv RST_STREAM -> closed
//
// reserved (local):
//   send HEADERS -> half-closed (remote)
//   send RST_STREAM -> closed
//   recv RST_STREAM -> closed
//
// open:
//   send END_STREAM -> half-closed (local)
//   recv END_STREAM -> half-closed (remote)
//   send RST_STREAM -> closed
//   recv RST_STREAM -> closed
//
// half-closed (remote):
//   send END_STREAM -> closed
//   send RST_STREAM -> closed
//   recv RST_STREAM -> closed
//
// half-closed (local):
//   recv END_STREAM -> closed
//   send RST_STREAM -> closed
//   recv RST_STREAM -> closed
//
// closed:
//   terminal
enum class stream_state
{
    idle,
    reserved_local,
    reserved_remote,
    open,
    half_closed_local,
    half_closed_remote,
    closed,
};

struct stream
{
    stream_state state;
    std::int64_t window;
};

struct connection
{
    std::unordered_map<std::uint32_t, stream> streams;

    // The following are from: https://httpwg.org/specs/rfc9113.html#SettingValues
    std::uint32_t header_table_size; // Default is 4096.
    bool          push_enabled;
    std::uint32_t max_concurrent_streams; // Default is no limit. Should be no smaller than 100. 0 is not special.
    std::uint32_t initial_window_size;    // Default is 65535.
    std::uint32_t max_frame_size;         // Default is 16384. Must be between that and 16777215.
    std::uint32_t max_header_list_size;   // Default is unlimited.
};

util::result<io::writer*, std::error_condition> request_encode(io::writer* writer, const client_request& req) noexcept
{}

util::result<io::writer*, std::error_condition> response_encode(io::writer*            writer,
                                                                const server_response& resp) noexcept
{}

util::result<server_request, std::error_condition> request_decode(std::unique_ptr<io::buffered_reader>&& reader,
                                                                  std::size_t max_header_bytes) noexcept
{}

util::result<client_response, std::error_condition> response_decode(std::unique_ptr<io::buffered_reader>&& reader,
                                                                    std::size_t max_header_bytes) noexcept
{}

}
