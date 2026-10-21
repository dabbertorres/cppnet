#include "http/priority.hpp"

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

#include "http/structured_fields.hpp"

namespace net::http
{

priority parse_priority(std::string_view value, priority base) noexcept
{
    // Each key's last member's value, if that's valid: a later member replaces an earlier one, valid or not.
    std::optional<std::uint8_t> urgency;
    std::optional<bool>         incremental;
    const auto                  on_member = [&](std::string_view key, const sf::member& m)
    {
        const auto* item = std::get_if<sf::item>(&m);
        if (key == "u")
        {
            const auto u = item != nullptr ? item->value.integer() : std::nullopt;
            urgency.reset();
            if (u && *u >= 0 && *u <= 7) urgency = static_cast<std::uint8_t>(*u);
        }
        else if (key == "i")
        {
            incremental = item != nullptr ? item->value.boolean() : std::nullopt;
        }
    };
    if (!sf::parse_dictionary(value, on_member)) return base;
    if (urgency) base.urgency = *urgency;
    if (incremental) base.incremental = *incremental;
    return base;
}

std::string to_string(const priority& p)
{
    std::string out;
    if (p.urgency != priority{}.urgency)
    {
        out += "u=";
        out += std::to_string(p.urgency);
    }
    if (p.incremental)
    {
        if (!out.empty()) out += ", ";
        out += 'i';
    }
    return out;
}

}
