#include "http/request.hpp"

#include <algorithm>
#include <cstddef>
#include <string_view>

#include "url.hpp"
#include "util/string_util.hpp"

namespace net::http
{

using namespace std::string_view_literals;

void server_request::add_path_value(std::string_view name, std::string_view encoded)
{
    if (!encoded.contains('%'))
    {
        path_values.emplace_back(name, encoded);
        return;
    }

    // NOTE: encoded is part of the path, and decoding doesn't lengthen anything, so with room for the rest of the path
    //       from the first value that needs it, none of them move. (Often, that fits in the string itself.)
    if (path_value_storage.empty())
    {
        const auto* path_end = target.path.data() + target.path.size();
        path_value_storage.reserve(static_cast<std::size_t>(path_end - encoded.data()));
    }

    const auto start = path_value_storage.size();
    url::decode_append(path_value_storage, encoded);
    path_values.emplace_back(name, std::string_view{path_value_storage}.substr(start));
}

detail::expectation detail::expectation_of(const server_request& req)
{
    // NOTE: once per request, so a request without one (nearly every request) only looks for it.
    if (!req.headers.get_all("Expect"sv).has_value()) return expectation::none;

    // RFC 9110, section 10.1.1: a server MUST ignore an HTTP/1.0 request's 100-continue, which HTTP/1.0 doesn't know
    // (nor any other).
    if (req.version.major == 1 && req.version.minor == 0) return expectation::none;

    const auto members = req.headers.get_list("Expect"sv);
    if (members.empty()) return expectation::none;

    const bool all_continue =
        std::ranges::all_of(members,
                            [](std::string_view member) { return util::equal_ignore_case(member, "100-continue"sv); });
    return all_continue ? expectation::continue_100 : expectation::unknown;
}

}
