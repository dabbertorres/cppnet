#pragma once

#include "http/http.hpp"
#include "encoding.hpp"

namespace net::http::_11
{

void response_encode(writer& w, const response& r);

}
