#pragma once

#include <iosfwd>
#include <streambuf>
#include <system_error>
#include <type_traits>

#include "result.hpp"

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

namespace net::http::http11
{

using util::result;

std::error_condition request_encode(std::ostream& w, const request& r) noexcept;
std::error_condition response_encode(std::ostream& w, const response& r) noexcept;

result<request, std::error_condition>  request_decode(std::istream& r) noexcept;
result<response, std::error_condition> response_decode(std::istream& r) noexcept;

class body_streambuf : public std::streambuf
{
public:
    body_streambuf(const std::streambuf& src) : std::streambuf{src} {}

private:
};

}
