#include "dns/error.hpp"

#include <string>
#include <system_error>

#include <netdb.h>

#include "exception.hpp"

namespace
{

using net::dns::condition;
using net::dns::errc;

class dns_category final : public std::error_category
{
public:
    [[nodiscard]] const char* name() const noexcept override { return "net::dns"; }

    [[nodiscard]] std::string message(int ev) const override
    {
        switch (static_cast<errc>(ev))
        {
        case errc::name_not_found: return "name not found";
        case errc::no_address: return "no address of the family asked for";
        case errc::server_failure: return "the name servers failed";
        case errc::refused: return "the name servers refused";
        case errc::bad_response: return "bad response from a name server";
        default: return "(unrecognized)";
        }
    }

    [[nodiscard]] std::error_condition default_error_condition(int ev) const noexcept override
    {
        switch (static_cast<errc>(ev))
        {
        case errc::name_not_found:
        case errc::no_address: return make_error_condition(condition::not_found);
        case errc::server_failure: return make_error_condition(condition::temporary_failure);
        default: return {ev, *this};
        }
    }
};

class condition_category final : public std::error_category
{
public:
    [[nodiscard]] const char* name() const noexcept override { return "net::dns::condition"; }

    [[nodiscard]] std::string message(int ev) const override
    {
        switch (static_cast<condition>(ev))
        {
        case condition::not_found: return "not found";
        case condition::temporary_failure: return "temporary failure";
        default: return "(unrecognized)";
        }
    }

    // NOTE: errc's codes match by their default_error_condition(), which the comparison also checks; this matches
    //       getaddrinfo()'s.
    [[nodiscard]] bool equivalent(const std::error_code& code, int cond) const noexcept override
    {
        if (code.category() != net::gai_category()) return false;

        const auto err = code.value();
        switch (static_cast<condition>(cond))
        {
        case condition::not_found:
#ifdef EAI_NODATA
            // NOTE: not a switch: EAI_NODATA may be EAI_NONAME's value, where it's kept for compatibility.
            if (err == EAI_NODATA) return true;
#endif
            return err == EAI_NONAME;

        case condition::temporary_failure: return err == EAI_AGAIN || err == EAI_FAIL;

        default: return false;
        }
    }
};

const dns_category       the_dns_category;
const condition_category the_condition_category;

}

namespace net::dns
{

const std::error_category& category() noexcept { return the_dns_category; }

std::error_code make_error_code(errc err) noexcept { return {static_cast<int>(err), the_dns_category}; }

const std::error_category& condition_category() noexcept { return the_condition_category; }

std::error_condition make_error_condition(condition cond) noexcept
{
    return {static_cast<int>(cond), the_condition_category};
}

}
