#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace net::http
{

// priority is an HTTP response's priority (RFC 9218): what a request's Priority field asks for, and what a response's
// may change. HTTP/2's server will send responses by it (docs/plans/http2.md, Priorities); a client asks for one with
// the request's field, e.g. req.headers.set("Priority", to_string({.urgency = 1})), and a handler sets its response's
// the same way (e.g. {.urgency = 7} for a bulk download), which the client also gets.
struct priority
{
    std::uint8_t urgency     = 3;     // 0 (most urgent) to 7 (background)
    bool         incremental = false; // whether the client uses the response as it arrives

    bool operator==(const priority&) const = default;
};

// parse_priority reads a Priority field's value over base: urgency from its u member, an Integer from 0 to 7, and
// incremental from its i member, a Boolean (a bare i is true). It's a Structured Fields Dictionary (RFC 9651), so a key
// that comes more than once is its last (RFC 9218 then judges that one: "u=1, u=9" leaves base's urgency); a member
// that's out of range or of another type is ignored (base's kept), as are members it doesn't know and parameters; and a
// value that isn't a Dictionary leaves base as it is. value is the field's lines joined with commas (RFC 9651, section
// 4.2: a field sent as several lines is one value): e.g. sf::join_lines(), in http/structured_fields.hpp.
[[nodiscard]] priority parse_priority(std::string_view value, priority base = {}) noexcept;

// to_string formats a priority as a Priority field's value, leaving out the defaults: "" for {3, false}, "u=5, i" for
// {5, true}, "i" for {3, true}. An urgency past 7 is written as it is (and ignored by whoever reads it).
[[nodiscard]] std::string to_string(const priority& p);

}
