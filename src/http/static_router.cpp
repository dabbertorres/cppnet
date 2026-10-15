#include "http/static_router.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "coro/task.hpp"
#include "http/request.hpp"
#include "http/response.hpp"

namespace
{

using net::http::detail::hash_byte;
using net::http::detail::hash_start;
using net::http::detail::mix;

constexpr int hex_value(char c) noexcept
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// decode_next decodes the byte at encoded[i], advancing i past it: as url::decode does, a '%' that isn't followed by
// two hex digits is kept as it is.
constexpr char decode_next(std::string_view encoded, std::size_t& i) noexcept
{
    if (encoded[i] == '%' && encoded.size() - i > 2)
    {
        const int high = hex_value(encoded[i + 1]);
        const int low  = hex_value(encoded[i + 2]);
        if (high >= 0 && low >= 0)
        {
            i += 3;
            return static_cast<char>((high << 4) | low);
        }
    }

    return encoded[i++];
}

// hash_decoded hashes encoded as it decodes, the same as hash_literal hashes the decoded segment.
constexpr std::uint32_t hash_decoded(std::string_view encoded, std::uint32_t seed) noexcept
{
    auto h = hash_start(seed);
    for (std::size_t i = 0; i < encoded.size();) h = hash_byte(h, decode_next(encoded, i));
    return mix(h);
}

// decodes_to is whether encoded decodes to literal.
constexpr bool decodes_to(std::string_view encoded, std::string_view literal) noexcept
{
    std::size_t at = 0;
    for (std::size_t i = 0; i < encoded.size();)
    {
        if (at == literal.size() || decode_next(encoded, i) != literal[at]) return false;
        ++at;
    }

    return at == literal.size();
}

}

namespace net::http::detail
{

match_result match(std::span<const node> nodes, std::span<const slot> slots, server_request& req)
{
    std::string_view path = req.target.path;
    if (path.starts_with('/')) path.remove_prefix(1);

    // the parameters are added as they're found, and removed if the path leads nowhere.
    const auto not_found = [&req] -> match_result
    {
        req.path_values.clear();
        req.path_value_storage.clear();
        return {};
    };

    // The path is split before it's decoded, so an encoded '/' ("%2F") is part of a segment rather than separating two.
    // A literal segment is taken over a parameter, and never given up for it later: there's no backtracking.
    const node* current = nodes.data();
    for (std::size_t start = 0; start <= path.size();)
    {
        auto end = path.find('/', start);
        if (end == std::string_view::npos) end = path.size();

        const auto segment = path.substr(start, end - start);
        start              = end + 1;

        if (current->slot_count != 0)
        {
            const auto& candidate =
                slots[current->first_slot + (hash_decoded(segment, current->seed) & (current->slot_count - 1))];

            if (candidate.child != none && decodes_to(segment, candidate.segment))
            {
                current = &nodes[candidate.child];
                continue;
            }
        }

        if (current->param_child == none || segment.empty()) return not_found();

        req.add_path_value(current->param_name, segment);
        current = &nodes[current->param_child];
    }

    // a node with no routes is only part of the way to some.
    if (current->pattern.empty()) return not_found();

    auto route = current->routes[static_cast<std::size_t>(req.method)];

    // HEAD is GET without the body (which the server drops), unless the route has its own handler for it.
    if (route == none && req.method == request_method::HEAD)
        route = current->routes[static_cast<std::size_t>(request_method::GET)];

    return {.found = current, .route = route};
}

std::string allowed_methods(const node& n)
{
    const auto handles = [&](request_method m) { return n.routes[static_cast<std::size_t>(m)] != none; };

    std::string out;
    for (std::size_t i = 0; i < static_cast<std::size_t>(request_method::NONE); ++i)
    {
        const auto method  = static_cast<request_method>(i);
        const bool allowed = handles(method) || (method == request_method::HEAD && handles(request_method::GET));
        if (!allowed) continue;

        if (!out.empty()) out += ", ";
        out += method_string(method);
    }

    return out;
}

coro::task<void> respond(response_writer& resp, status status_code) { co_await resp.send(status_code, 0); }

}
