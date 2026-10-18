// fuzz_request_target checks that parse_request_target doesn't crash, and that:
// - normalizing a path is idempotent, and leaves no dot segments.
// - what the client sends (url::origin_form()) is always a valid target.
#include "http/request_target.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <print>
#include <string>
#include <string_view>

#include "http/request.hpp"
#include "url.hpp"

using net::http::parse_request_target;
using net::http::request_method;

namespace
{

[[noreturn]] void fail(const char* what, std::string_view a, std::string_view b)
{
    std::println(stderr, "PROPERTY FAILED: {}\n  input: [{}]\n  got:   [{}]", what, a, b);
    std::abort();
}

}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    std::string_view input{reinterpret_cast<const char*>(data), size};

    for (auto method : {request_method::GET, request_method::CONNECT, request_method::OPTIONS})
    {
        auto target = parse_request_target(method, input);
        if (!target) continue;
        (void)target->query_values();

        if (target->kind == net::http::request_target::form::origin
            || target->kind == net::http::request_target::form::absolute)
        {
            // normalizing is idempotent, and leaves no dot segments.
            auto again = parse_request_target(request_method::GET, target->path);
            if (!again || again->path != target->path)
                fail("path normalization is not idempotent", input, target->path);

            std::string_view p = target->path;
            if (p.contains("/./") || p.contains("/../") || p.ends_with("/.") || p.ends_with("/.."))
                fail("dot segment left in path", input, target->path);
        }
    }

    // whatever the client sends is a valid origin-form target.
    auto parsed = net::url::parse(input);
    if (parsed.has_value())
    {
        auto origin = parsed.value().origin_form();
        if (!parse_request_target(request_method::GET, origin))
            fail("origin_form produced an invalid target", input, origin);
    }

    return 0;
}
