#include "http/request.hpp"

#include <cstddef>
#include <string_view>

#include "url.hpp"

namespace net::http
{

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

}
