#include "io/io.hpp"

#include <string>
#include <system_error>

namespace
{

class io_category final : public std::error_category
{
public:
    [[nodiscard]] const char* name() const noexcept override { return "net::io"; }

    [[nodiscard]] std::string message(int ev) const override
    {
        switch (static_cast<net::io::errc>(ev))
        {
        case net::io::errc::end_of_stream: return "end of stream";
        case net::io::errc::no_progress: return "a reader or writer made no progress";
        default: return "(unrecognized)";
        }
    }

    [[nodiscard]] std::error_condition default_error_condition(int ev) const noexcept override
    {
        if (static_cast<net::io::errc>(ev) == net::io::errc::end_of_stream)
            return make_error_condition(net::io::status_condition::closed);
        return {ev, *this};
    }
};

class status_condition_category final : public std::error_category
{
public:
    [[nodiscard]] const char* name() const noexcept override { return "net::io::status_condition"; }

    [[nodiscard]] std::string message(int ev) const override
    {
        switch (static_cast<net::io::status_condition>(ev))
        {
        case net::io::status_condition::closed: return "closed";
        case net::io::status_condition::timed_out: return "timed out";
        default: return "(unrecognized)";
        }
    }

    // NOTE: codes in other categories (e.g. io::errc's) match by their default_error_condition(), which the comparison
    // also checks; this matches the OS's.
    [[nodiscard]] bool equivalent(const std::error_code& code, int condition) const noexcept override
    {
        if (code.category() != std::system_category() && code.category() != std::generic_category()) return false;

        const auto err = static_cast<std::errc>(code.value());
        switch (static_cast<net::io::status_condition>(condition))
        {
        case net::io::status_condition::closed:
            switch (err)
            {
            case std::errc::broken_pipe:
            case std::errc::connection_aborted:
            case std::errc::connection_reset:
            case std::errc::network_reset:
            case std::errc::not_connected: return true;
            default: return false;
            }

        case net::io::status_condition::timed_out: return err == std::errc::timed_out;

        default: return false;
        }
    }
};

const io_category               the_io_category;
const status_condition_category the_status_condition_category;

}

namespace net::io
{

const std::error_category& category() noexcept { return the_io_category; }

std::error_code make_error_code(errc err) noexcept { return {static_cast<int>(err), the_io_category}; }

const std::error_category& status_condition_category() noexcept { return the_status_condition_category; }

std::error_condition make_error_condition(status_condition cond) noexcept
{
    return {static_cast<int>(cond), the_status_condition_category};
}
}
