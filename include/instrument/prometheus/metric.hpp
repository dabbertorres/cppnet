#pragma once

#include <atomic>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "coro/task.hpp"
#include "io/io.hpp"
#include "io/writer.hpp"
#include "util/hash.hpp"

namespace net::instrument::prometheus
{

using namespace std::string_view_literals;

enum class metric_type
{
    counter,
    gauge,
    histogram,
    /* summary, */
    untyped,
};

std::string_view metric_type_string(metric_type type) noexcept;

// format_value formats a sample's value (or a histogram bucket's bound) as the text format expects: in as few digits as
// read back as the same double, or as +Inf, -Inf or NaN.
std::string format_value(double value);

template<typename T>
struct base_metric;

// clang-format off
template<typename T>
concept metric_impl = std::derived_from<T, base_metric<T>>
    && requires
    {
        { T::type() } -> std::same_as<metric_type>;
    };
// clang-format on

struct metric_label
{
    std::string key;
    std::string val;

    friend auto operator<=>(const metric_label& lhs, const metric_label& rhs) noexcept = default;
};

using metric_labels = std::vector<metric_label>;

namespace detail
{

// add adds v to value, as one atomic step, and returns the result.
// NOTE: rather than fetch_add, which not every standard library has for floating-point atomics yet.
inline double add(std::atomic<double>& value, double v) noexcept
{
    auto current = value.load(std::memory_order_relaxed);
    while (!value.compare_exchange_weak(current, current + v, std::memory_order_relaxed))
    {
    }

    return current + v;
}

// append_escaped appends text to out, escaping it for HELP text (backslashes and newlines) or, if label_value, for a
// label's value (also double quotes).
void append_escaped(std::string& out, std::string_view text, bool label_value);

// write_text writes all of text, as a write may be partial.
coro::task<io::result> write_text(io::writer& out, std::string text);

}

template<typename T>
struct base_metric
{
    base_metric(std::string&& name, std::string&& help = "", metric_labels&& labels = {})
        : name{std::move(name)}
        , help{std::move(help)}
        , labels{std::move(labels)}
    {}

    base_metric(const base_metric& other)
        : name{other.name}
        , help{other.help}
        , labels{other.labels}
        , last_update{other.last_update.load(std::memory_order_acquire)}
    {}

    base_metric& operator=(const base_metric& other)
    {
        if (this == std::addressof(other)) return *this;

        name   = other.name;
        help   = other.help;
        labels = other.labels;
        last_update.exchange(other.last_update, std::memory_order_acquire);

        return *this;
    }

    base_metric(base_metric&& other) noexcept
        : name{std::exchange(other.name, "")}
        , help{std::exchange(other.help, "")}
        , labels{std::exchange(other.labels, {})}
        , last_update{other.last_update.exchange(std::chrono::seconds(0), std::memory_order_acquire)}
    {}

    base_metric& operator=(base_metric&& other) noexcept
    {
        name   = std::exchange(other.name, "");
        help   = std::exchange(other.help, "");
        labels = std::exchange(other.labels, {});
        last_update.store(other.last_update.exchange(std::chrono::seconds(0), std::memory_order_acquire),
                          std::memory_order_release);

        return *this;
    }

    ~base_metric() = default;

    std::string   name;
    std::string   help;
    metric_labels labels;

    template<typename Clock = std::chrono::steady_clock>
    [[nodiscard]] Clock::time_point last_updated() const noexcept
    {
        auto since_epoch       = last_update.load(std::memory_order_acquire);
        auto clock_since_epoch = std::chrono::duration_cast<Clock::duration>(since_epoch);
        return std::chrono::time_point<Clock>{clock_since_epoch};
    }

    template<typename Clock = std::chrono::steady_clock>
    void set_updated_now() noexcept
    {
        auto ts = Clock::now();
        set_updated_at(ts);
    }

    template<typename Clock = std::chrono::steady_clock, typename Duration = Clock::duration>
    void set_updated_at(std::chrono::time_point<Clock, Duration> timepoint) noexcept
    {
        auto since_epoch = timepoint.time_since_epoch();
        auto seconds     = std::chrono::duration_cast<decltype(last_update.load())>(since_epoch);
        last_update.store(seconds, std::memory_order_relaxed);
    }

    // encode writes the metric in Prometheus' text format.
    coro::task<io::result> encode(io::writer& out) const
    {
        std::string text;
        encode_to(text);
        co_return co_await detail::write_text(out, std::move(text));
    }

    // encode_to appends the metric, in Prometheus' text format, to out.
    //
    // NOTE: the text is built synchronously, and only then written, so nothing it's made from need outlive a
    //       suspension (e.g. temporaries, held by reference in a lazy task's frame), and it's a snapshot of the metric.
    void encode_to(std::string& out) const
    {
        const auto* self = static_cast<const T*>(this);

        // NOTE: checked here, rather than with a concept, as this is a friend of T, whose encode_self is private.
        if constexpr (requires { self->encode_self(out); })
        {
            self->encode_self(out);
        }
        else
        {
            static_assert(requires { { self->get() } -> std::convertible_to<double>; },
                          "a metric needs an encode_self, or a get() of its value");

            // NOTE: no timestamp, as Prometheus then uses the time of the scrape. (The text format's are milliseconds
            //       since the Unix epoch, which last_update, from steady_clock, isn't.)
            encode_help(out);
            encode_type(out);
            out += name;
            encode_labels(out);
            out += ' ';
            out += format_value(self->get());
            out += '\n';
        }
    }

protected:
    void encode_help(std::string& out) const
    {
        if (help.empty()) return;

        out += "# HELP ";
        out += name;
        out += ' ';
        detail::append_escaped(out, help, false);
        out += '\n';
    }

    void encode_type(std::string& out) const
    {
        out += "# TYPE ";
        out += name;
        out += ' ';
        out += metric_type_string(T::type());
        out += '\n';
    }

    // encode_labels appends the labels, if any, e.g. {a="1",b="2"}. A first label (e.g. a histogram bucket's "le") is
    // written before them, if first_key isn't empty.
    void encode_labels(std::string& out, std::string_view first_key = {}, std::string_view first_value = {}) const
    {
        if (labels.empty() && first_key.empty()) return;

        out += '{';

        bool first = true;
        auto label = [&](std::string_view key, std::string_view value)
        {
            if (!first) out += ',';
            first = false;

            out += key;
            out += R"(=")";
            detail::append_escaped(out, value, true);
            out += '"';
        };

        if (!first_key.empty()) label(first_key, first_value);
        for (const auto& [key, value] : labels) label(key, value);

        out += '}';
    }

    std::atomic<std::chrono::seconds> last_update;
};

// clang-format off
template<typename T>
concept derive_hook = metric_impl<T>
    && requires(T& parent, T& child)
    {
        { parent.on_derive(child) };
    };
// clang-format on

template<metric_impl T>
T derive_child(const T& parent, const metric_labels& new_labels)
{
    T child{parent.name, parent.help, parent.labels};

    for (const auto& [k, v] : new_labels)
    {
        child.labels.try_emplace(k, v);
    }

    if constexpr (derive_hook<T>) parent.derive_hook(child);

    return child;
}

}

namespace std
{

template<>
struct hash<net::instrument::prometheus::metric_label>
{
    std::size_t operator()(const net::instrument::prometheus::metric_label& value) const noexcept
    {
        std::size_t key_hash = net::util::detail::hash_combine(0, value.key);
        return net::util::detail::hash_combine(key_hash, value.val);
    }
};

}
