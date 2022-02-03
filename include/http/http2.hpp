#pragma once

#include "util.hpp"

namespace net
{

class reader;
class writer;

}

namespace net::http
{

struct request;
struct response;

}

namespace net::http::http2
{

using util::result;

using TODO = std::void_t<>;

result<std::void_t<>, std::string> response_encode(writer& w, const response& r) noexcept;
result<std::void_t<>, std::string> request_encode(writer& w, const request& r) noexcept;

result<response, TODO> response_decode(reader& r) noexcept;
result<request, TODO>  request_decode(reader& r) noexcept;

}
