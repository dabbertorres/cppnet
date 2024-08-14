#include "io/io.hpp"

#include <string>
#include <system_error>

namespace
{

struct status_condition_category : std::error_category
{
    [[nodiscard]] const char* name() const noexcept override { return "net::io::status_condition"; }
    [[nodiscard]] std::string message(int ev) const override
    {
        switch (static_cast<net::io::status_condition>(ev))
        {
        case net::io::status_condition::closed: return "closed";
        case net::io::status_condition::timed_out: return "timed out";
        case net::io::status_condition::error: return "error";
        default: return "(unrecognized)";
        }
    }

    [[nodiscard]] bool equivalent(const std::error_code& code, int condition) const noexcept override
    {
        auto code_value = static_cast<std::errc>(code.value());

        switch (static_cast<net::io::status_condition>(condition))
        {
        case net::io::status_condition::closed:
            switch (code_value)
            {
            case std::errc::broken_pipe:
            case std::errc::connection_aborted:
            case std::errc::connection_refused:
            case std::errc::host_unreachable:
            case std::errc::interrupted:
            case std::errc::io_error:
            case std::errc::network_down:
            case std::errc::network_reset:
            case std::errc::network_unreachable:
            case std::errc::no_message_available:
            case std::errc::no_message:
            case std::errc::no_stream_resources:
            case std::errc::no_such_device_or_address:
            case std::errc::no_such_device:
            case std::errc::no_such_process:
            case std::errc::not_a_socket:
            case std::errc::not_a_stream:
            case std::errc::not_connected:
            case std::errc::operation_canceled:
            case std::errc::owner_dead:
            case std::errc::state_not_recoverable: return true;

            default: return false;
            }

        case net::io::status_condition::timed_out:
            switch (code_value)
            {
            case std::errc::timed_out:
            case std::errc::stream_timeout: return true;

            default: return false;
            }

        case net::io::status_condition::error: return code_value != std::errc{};

        default: return false;
        }
    }
};

const status_condition_category the_status_condition_category;

}

namespace net::io
{

std::error_condition make_error_condition(status_condition code)
{
    return {static_cast<int>(code), the_status_condition_category};
}

const std::error_category& status_condition_category() noexcept { return the_status_condition_category; }

}
