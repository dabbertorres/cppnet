#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <list>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <type_traits>
#include <unordered_map>

namespace net::util
{

// cache provides a data cache structure using the SIEVE algorithm.
template<typename K, typename V>
class cache
{
public:
    cache(std::size_t capacity)
        : capacity{capacity}
        , hand{entries.end()}
    {
        lookup.reserve(capacity);
    }

    void set(K key, V val)
    {
        std::unique_lock lock{mu};

        auto it = lookup.find(key);
        if (it != lookup.end())
        {
            it->second->val = val;
            it->second->visited.test_and_set(std::memory_order_release);
            return;
        }

        if (entries.size() >= capacity) evict();

        entry ent{key, val};
        entries.push_front(ent);
        lookup[key] = entries.begin();
    }

    std::optional<V> get(K key) noexcept
    {
        std::shared_lock lock{mu};

        auto it = lookup.find(key);
        if (it != lookup.end())
        {
            it->second->visited.test_and_set(std::memory_order_release);
            return it->second->val;
        }

        return std::nullopt;
    }

    template<typename F>
        requires(std::is_invocable_r_v<V, F, K>)
    V get_or_set(K key, F&& f)
    {
        if (auto opt = get(key); opt.has_value()) return opt.value();

        auto result = std::invoke(std::forward<F>(f), key);

        std::unique_lock lock{mu};
        set(key, result);
        return result;
    }

    bool erase(K key) noexcept
    {
        std::unique_lock lock{mu};

        auto it = lookup.find(key);
        if (it != lookup.end())
        {
            if (it->second == hand) hand = std::prev(hand);

            entries.erase(it->second);
            lookup.erase(it);
            return true;
        }

        return false;
    }

    bool contains(K key) const noexcept
    {
        std::shared_lock lock{mu};

        auto it = lookup.find(key);
        return it != lookup.end();
    }

    [[nodiscard]] std::size_t size() const noexcept { return entries.size(); }
    [[nodiscard]] bool        empty() const noexcept { return entries.empty(); }

    void purge()
    {
        std::unique_lock lock{mu};

        entries.clear();
        lookup.clear();
    }

private:
    struct entry
    {
        entry(K key, V val)
            : key{key}
            , val{val}
        {}

        entry(const entry& other)
            : key{other.key}
            , val{other.val}
        {
            if (other.visited.test()) visited.test_and_set();
        }

        entry(entry&& other) noexcept
            : key{std::move(other.key)}
            , val{std::move(other.val)}
        {
            if (other.visited.test()) visited.test_and_set();
        }

        entry& operator=(const entry&) = delete;
        entry& operator=(entry&&)      = delete;

        ~entry() = default;

        K                key;
        V                val;
        std::atomic_flag visited;
    };

    using entry_list   = std::list<entry>;
    using lookup_table = std::unordered_map<K, typename entry_list::iterator>;

    void evict()
    {
        auto old = hand == entries.end() ? std::prev(hand) : hand;

        while (old->visited.test(std::memory_order_acquire))
        {
            old->visited.clear(std::memory_order_release);

            old = old == entries.begin() ? std::prev(entries.end()) : std::prev(old);
        }

        hand = std::prev(old);
        lookup.erase(old->key);
        entries.erase(old);
    }

    std::size_t          capacity;
    std::shared_mutex    mu;
    lookup_table         lookup;
    entry_list           entries;
    entry_list::iterator hand;
};

}
