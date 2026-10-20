// fuzz_url checks that url's functions don't crash (they're noexcept, so exceptions count), and that:
// - build() output parses back to the same url.
// - build() and origin_form() output only printable ASCII, so nothing in a url can end an HTTP request line.
// - resolve() makes a url that build() round-trips, and whose build() resolves to it against any base (but for the
//   base's scheme, if it has none; and unless its path has dot segments, which resolving removes: only an empty
//   reference's path, the base's, can have them).
// The input is a URL; or, with a '\n' in it, a base URL before it and a reference after it (otherwise the reference
// is resolved against RFC 3986's example base).
#include "url.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <print>
#include <string>
#include <string_view>

namespace
{

[[noreturn]] void fail(const char* what, std::string_view input, std::string_view got)
{
    std::println(stderr, "PROPERTY FAILED: {}\n  input: [{}]\n  got:   [{}]", what, input, got);
    std::abort();
}

void check_printable(const char* what, std::string_view input, std::string_view out)
{
    for (unsigned char c : out)
        if (c <= 0x20 || c >= 0x7F) fail(what, input, out);
}

bool has_dot_segment(std::string_view path)
{
    while (true)
    {
        const auto slash   = path.find('/');
        const auto segment = path.substr(0, slash);
        if (segment == "." || segment == "..") return true;
        if (slash == std::string_view::npos) return false;
        path.remove_prefix(slash + 1);
    }
}

void check_resolve(std::string_view input, const net::url& base, std::string_view reference)
{
    auto resolved = net::url::resolve(base, reference);
    if (!resolved.has_value()) return;

    const auto& r     = resolved.value();
    auto        built = r.build();
    check_printable("resolve()'s build() emitted a control, space, or non-ASCII byte", input, built);

    auto reparsed = net::url::parse(built);
    if (!reparsed.has_value()) fail("resolve()'s build() output doesn't parse", input, built);
    if (!(reparsed.value() == r)) fail("resolve()'s build() doesn't round-trip", input, built);

    if (has_dot_segment(r.path)) return;

    static const auto other = net::url::parse("https://other.example/x/y?z#w").value();
    for (const auto* b : {&base, &other})
    {
        auto again = net::url::resolve(*b, built);
        if (!again.has_value()) fail("resolve()'s build() isn't a valid reference", input, built);
        // (one without a scheme, as a base with an authority but no scheme gives, takes the base's.)
        auto expect = r;
        if (expect.scheme.empty()) expect.scheme = b->scheme;
        if (!(again.value() == expect)) fail("resolve()'s build() doesn't resolve to itself", input, again->build());
    }
}

}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    std::string_view input{reinterpret_cast<const char*>(data), size};

    (void)net::url::decode(input);
    (void)net::url::encode(input);

    if (auto newline = input.find('\n'); newline != std::string_view::npos)
    {
        auto base = net::url::parse(input.substr(0, newline));
        if (base.has_value()) check_resolve(input, base.value(), input.substr(newline + 1));
    }
    else
    {
        static const auto rfc_base = net::url::parse("http://a/b/c/d;p?q").value();
        check_resolve(input, rfc_base, input);
    }

    auto parsed = net::url::parse(input);
    if (!parsed.has_value()) return 0;

    const auto& u     = parsed.value();
    auto        built = u.build();
    check_printable("build() emitted a control, space, or non-ASCII byte", input, built);

    auto reparsed = net::url::parse(built);
    if (!reparsed.has_value()) fail("build() output doesn't parse", input, built);
    if (!(reparsed.value() == u)) fail("build() doesn't round-trip", input, built);

    auto origin = u.origin_form();
    check_printable("origin_form() emitted a control, space, or non-ASCII byte", input, origin);

    return 0;
}
