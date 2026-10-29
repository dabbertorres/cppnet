#include "websocket/error.hpp"

#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <system_error>

#include "io/io.hpp"

namespace
{

using net::websocket::close_code;
using net::websocket::errc;
using net::websocket::sent_base;

// code_name is a close code's meaning, as RFC 6455 (section 7.4.1) and IANA's registry name it.
std::string code_name(int code)
{
    using enum close_code;

    switch (static_cast<close_code>(code))
    {
    case normal_closure: return "normal closure";
    case going_away: return "going away";
    case protocol_error: return "protocol error";
    case unsupported_data: return "unsupported data";
    case no_status_received: return "closed without a status code";
    case abnormal_closure: return "the connection ended without a close";
    case invalid_payload: return "invalid payload data";
    case policy_violation: return "policy violation";
    case message_too_big: return "message too big";
    case mandatory_extension: return "mandatory extension";
    case internal_error: return "internal error";
    case service_restart: return "service restart";
    case try_again_later: return "try again later";
    case bad_gateway: return "bad gateway";
    }
    return std::format("close code {}", code);
}

// closes are the codes that only say the connection ended, whoever's they are.
bool closes(int code) noexcept
{
    using enum close_code;

    switch (static_cast<close_code>(code))
    {
    case normal_closure:
    case going_away:
    case no_status_received:
    case abnormal_closure: return true;
    default: return false;
    }
}

class close_category_impl final : public std::error_category
{
public:
    [[nodiscard]] const char* name() const noexcept override { return "websocket::close"; }

    [[nodiscard]] std::string message(int code) const override
    {
        if (code >= 0 && code < sent_base) return code_name(code) + " (from the peer)";
        if (code >= sent_base && code <= sent_base + 0xffff) return code_name(code - sent_base) + " (sent to the peer)";
        return "unknown WebSocket close";
    }

    [[nodiscard]] std::error_condition default_error_condition(int code) const noexcept override
    {
        if (closes(code >= sent_base ? code - sent_base : code))
            return make_error_condition(net::io::status_condition::closed);
        return {code, *this};
    }
};

class websocket_category final : public std::error_category
{
public:
    [[nodiscard]] const char* name() const noexcept override { return "websocket"; }

    [[nodiscard]] std::string message(int code) const override
    {
        switch (static_cast<errc>(code))
        {
        case errc::not_websocket: return "not a WebSocket handshake";
        case errc::bad_version: return "unsupported WebSocket version (only 13 is)";
        case errc::bad_key: return "bad Sec-WebSocket-Key";
        case errc::origin_refused: return "the request's origin isn't allowed";
        case errc::bad_status: return "the server didn't switch protocols";
        case errc::bad_accept: return "bad Sec-WebSocket-Accept";
        case errc::bad_subprotocol: return "the server chose a subprotocol that wasn't offered";
        case errc::bad_extension: return "the server chose an extension that wasn't offered";
        }
        return "unknown WebSocket error";
    }
};

const close_category_impl the_close_category;
const websocket_category  the_category;

}

namespace net::websocket
{

const std::error_category& close_category() noexcept { return the_close_category; }

std::error_code make_error_code(close_code code) noexcept { return {static_cast<int>(code), the_close_category}; }

std::error_code sent(close_code code) noexcept { return {sent_base + static_cast<int>(code), the_close_category}; }

bool from_peer(std::error_code err) noexcept
{
    return err.category() == the_close_category && err.value() >= 0 && err.value() < sent_base;
}

std::optional<close_code> close_code_of(std::error_code err) noexcept
{
    if (err.category() != the_close_category) return std::nullopt;

    const auto code = err.value() >= sent_base ? err.value() - sent_base : err.value();
    if (code < 0 || code > 0xffff) return std::nullopt;
    return static_cast<close_code>(static_cast<std::uint16_t>(code));
}

const std::error_category& category() noexcept { return the_category; }

std::error_code make_error_code(errc err) noexcept { return {static_cast<int>(err), the_category}; }

}
