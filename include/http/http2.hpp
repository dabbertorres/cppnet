#pragma once

#include <cstddef>
#include <string>

#include "io/reader.hpp"
#include "io/writer.hpp"
#include "util/result.hpp"

namespace net::http
{

struct request;
struct server_response;

}

namespace net::http::http2
{

using util::result;

using TODO = std::void_t<>;

result<std::void_t<>, std::string> response_encode(io::writer<std::byte>& w, const server_response& r) noexcept;
result<std::void_t<>, std::string> request_encode(io::writer<std::byte>& w, const request& r) noexcept;

result<server_response, TODO> response_decode(io::reader<std::byte>& r) noexcept;
result<request, TODO>         request_decode(io::reader<std::byte>& r) noexcept;

}
