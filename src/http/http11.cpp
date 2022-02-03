#include "http/http11.hpp"

#include "encoding.hpp"

namespace net::http::_11
{

void response_encode(writer& w, const response& r)
{
    for (auto& kv : r.headers)
    {
        w << kv.first << ": " << kv.second << "\r\n";
    }
}

}
