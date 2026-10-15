#include "instrument/prometheus/metric.hpp"

#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "coro/task.hpp"
#include "io/io.hpp"
#include "io/writer.hpp"

namespace net::instrument::prometheus
{

using namespace std::chrono_literals;
using namespace std::string_view_literals;

std::string_view metric_type_string(metric_type type) noexcept
{
    switch (type)
    {
    case metric_type::counter: return "counter"sv;
    case metric_type::gauge: return "gauge"sv;
    case metric_type::histogram: return "histogram"sv;
    /* case metric_type::summary: return "summary"sv; */
    case metric_type::untyped: return "untyped"sv;
    }
}

std::string format_value(double value)
{
    if (std::isnan(value)) return "NaN";
    if (std::isinf(value)) return value > 0 ? "+Inf" : "-Inf";

    // as few digits as read back as the same double: in plain decimal (e.g. 400000 or 0.005, rather than 4e+05) unless
    // it's very large or small (e.g. 1e-07). NOTE: at most 22 digits, plus a sign and point, or 24 characters.
    const auto magnitude = std::abs(value);
    const auto format    = magnitude == 0 || (magnitude >= 1e-4 && magnitude < 1e21) ? std::chars_format::fixed
                                                                                      : std::chars_format::scientific;

    std::array<char, 32> buf{};
    auto [end, _] = std::to_chars(buf.data(), buf.data() + buf.size(), value, format);
    return {buf.data(), end};
}

namespace detail
{

void append_escaped(std::string& out, std::string_view text, bool label_value)
{
    for (char c : text)
    {
        if (c == '\\') out += R"(\\)";
        else if (c == '\n') out += R"(\n)";
        else if (c == '"' && label_value) out += R"(\")";
        else out += c;
    }
}

coro::task<io::result> write_text(io::writer& out, std::string text)
{
    std::span<const char> remaining{text};
    std::size_t           total = 0;

    while (!remaining.empty())
    {
        auto res = co_await out.write(remaining);
        total += res.count;
        if (res.err) co_return {.count = total, .err = res.err};

        remaining = remaining.subspan(res.count);
    }

    co_return {.count = total, .err = {}};
}

}

}
