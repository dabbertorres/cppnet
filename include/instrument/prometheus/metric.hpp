#pragma once

#include <atomic>
#include <chrono>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "io/writer.hpp"
#include "util/hash.hpp"
#include "util/string_util.hpp"

namespace net::instrument::prometheus
{

enum class metric_type
{
    counter,
    gauge,
    histogram,
    /* summary, */
    untyped,
};

std::string_view metric_type_string(metric_type type) noexcept;

template<typename T>
struct base_metric;

template<typename T>
concept self_encoder = requires(const T* t, io::writer& out)
// clang-format off
{
    { t->encode_self(out) } -> std::same_as<io::result>;
};
// clang-format on

template<typename T>
concept value_encoder = requires(const T* t, io::writer& out)
// clang-format off
{
    { t->encode_value(out) } -> std::same_as<io::result>;
};
// clang-format on

// clang-format off
template<typename T>
concept metric_impl = std::derived_from<T, base_metric<T>>
    && (self_encoder<T> || value_encoder<T>)
    && requires(const T* t, io::writer& out)
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

template<typename T>
struct base_metric
{
    base_metric(std::string name, std::string help = "", metric_labels&& labels = {})
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

    io::result encode(io::writer& out) const
    {
        if constexpr (self_encoder<T>)
        {
            return static_cast<const T*>(this)->encode_self(out);
        }
        else if constexpr (value_encoder<T>)
        {
            return io::write_all(
                out,
                [this](io::writer& out) { return encode_help(out); },
                [this](io::writer& out) { return encode_type(out); },
                name,
                [this](io::writer& out) { return encode_labels(out); },
                ' ',
                [this](io::writer& out) { return static_cast<const T*>(this)->encode_value(out); },
                ' ',
                [this](io::writer& out) { return encode_timestamp(out); },
                '\n');
        }
    }

protected:
    io::result encode_help(io::writer& out) const
    {
        if (!help.empty())
        {
            return io::write_all(out, "# HELP ", name, ' ', help, '\n');
        }

        return {};
    }

    io::result encode_type(io::writer& out) const
    {
        return io::write_all(out, "# TYPE ", name, ' ', metric_type_string(T::type()), '\n');
    }

    io::result encode_labels(io::writer& out) const
    {
        if (!labels.empty())
        {
            return io::write_all(
                out,
                '{',
                [this](io::writer& out) { return encode_all_labels(out); },
                '}');
        }

        return {};
    }

    io::result encode_all_labels(io::writer& out) const
    {
        std::size_t total = 0;

        for (const auto& [label_name, label_value] : labels)
        {
            auto res = encode_one_label(out, label_name, label_value);
            total += res.count;
            if (res.err) return {.count = total, .err = res.err};
        }

        return {.count = total};
    }

    io::result encode_one_label(io::writer& out, std::string_view label_name, std::string_view label_value) const
    {
        return io::write_all(
            out,
            label_name,
            "=\"",
            [=](io::writer& out)
            {
                // clang-format off
                return util::replace_all_to(out, label_value, {
                    {R"(\)", R"(\\)"},
                    {R"(")", R"(\")"},
                    { "\n",  R"(\n)"},
                });
                // clang-format on
            },
            "\",");
    }

    io::result encode_timestamp(io::writer& out) const
    {
        auto ts = last_update.load(std::memory_order_acquire);
        return out.write(std::to_string(ts.count()));
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
T derive_child(const T& parent, metric_labels&& new_labels)
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
