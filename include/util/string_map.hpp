#pragma once

#include <concepts>
#include <cstddef>
#include <functional>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>

#ifdef __has_include
#    if __has_include(<ranges>)
#        include <ranges>
#    endif

#    if __has_include(<version>)
#        include <version>
#    endif
#endif

namespace net::util
{

#if defined(__cpp_lib_ranges_to_container) && __cpp_lib_ranges_to_container >= 202202L
template<typename R, typename T>
concept container_compatible_range =
    std::ranges::input_range<R> && std::convertible_to<std::ranges::range_reference_t<R>, T>;
#endif

template<typename T, typename String = std::string, typename StringView = std::string_view>
concept string_map_hasher = requires(T t, const String& s, StringView sv)
// clang-format off
{
    { typename T::is_transparent() };
    { t(s) }                         -> std::same_as<std::size_t>;
    { t(sv) }                        -> std::same_as<std::size_t>;
};
// clang-format on

template<typename String = std::string, typename StringView = std::string_view>
struct default_string_map_hasher
{
    using hash_type      = typename std::hash<StringView>;
    using is_transparent = void;

    std::size_t operator()(const String& key) const noexcept { return hash_type{}(key); }
    std::size_t operator()(StringView key) const noexcept { return hash_type{}(key); }
};

static_assert(string_map_hasher<default_string_map_hasher<>>);

template<typename T, typename String = std::string, typename StringView = std::string_view>
concept string_map_keyequal = requires(T t, const String& s, StringView sv)
// clang-format off
{
    { typename T::is_transparent() };
    { t(s, s) }                      -> std::same_as<bool>;
    { t(sv, s) }                     -> std::same_as<bool>;
    { t(s, sv) }                     -> std::same_as<bool>;
};
// clang-format on

template<typename String = std::string, typename StringView = std::string_view>
struct default_string_map_keyequal
{
    using is_transparent = void;

    bool operator()(const String& lhs, const String& rhs) const noexcept { return lhs == rhs; }
    bool operator()(StringView lhs, const String& rhs) const noexcept { return lhs == rhs; }
    bool operator()(const String& lhs, StringView rhs) const noexcept { return lhs == rhs; }
};

static_assert(string_map_keyequal<default_string_map_keyequal<>>);

template<typename T,
         typename String              = std::string,
         typename StringView          = std::string_view,
         string_map_hasher   Hash     = default_string_map_hasher<String, StringView>,
         string_map_keyequal KeyEqual = default_string_map_keyequal<String, StringView>,
         typename Allocator           = std::allocator<std::pair<const String, T>>>
class string_map
{
public:
    using mapped_type          = T;
    using key_type             = String;
    using map_type             = std::unordered_map<key_type, mapped_type, Hash, KeyEqual>;
    using value_type           = map_type::value_type;
    using size_type            = map_type::size_type;
    using difference_type      = map_type::difference_type;
    using allocator_type       = Allocator;
    using reference            = map_type::reference;
    using const_reference      = map_type::const_reference;
    using pointer              = map_type::pointer;
    using const_pointer        = map_type::const_pointer;
    using iterator             = map_type::iterator;
    using const_iterator       = map_type::const_iterator;
    using local_iterator       = map_type::local_iterator;
    using const_local_iterator = map_type::const_local_iterator;
    using node_type            = map_type::node_type;
    using insert_return_type   = map_type::insert_return_type;

    string_map() = default;

    explicit string_map(size_type             bucket_count,
                        const Hash&           hash  = Hash{},
                        const KeyEqual&       equal = KeyEqual{},
                        const allocator_type& alloc = allocator_type{})
        : values(bucket_count, hash, equal, alloc)
    {}

    string_map(size_type bucket_count, const allocator_type& alloc)
        : values(bucket_count, Hash{}, KeyEqual{}, alloc)
    {}

    string_map(size_type bucket_count, const Hash& hash, const allocator_type& alloc)
        : values(bucket_count, hash, KeyEqual{}, alloc)
    {}

    explicit string_map(const allocator_type& alloc)
        : values(alloc)
    {}

    template<typename InputIt>
    string_map(InputIt first, InputIt last)
        : values(first, last)
    {}

    template<typename InputIt>
    string_map(InputIt               first,
               InputIt               last,
               size_type             bucket_count,
               const Hash&           hash  = Hash{},
               const KeyEqual&       equal = KeyEqual{},
               const allocator_type& alloc = allocator_type{})
        : values(first, last, bucket_count, hash, equal, alloc)
    {}

    template<typename InputIt>
    string_map(InputIt first, InputIt last, size_type bucket_count, const allocator_type& alloc)
        : values(first, last, bucket_count, Hash{}, KeyEqual{}, alloc)
    {}

    template<typename InputIt>
    string_map(InputIt first, InputIt last, size_type bucket_count, const Hash& hash, const allocator_type& alloc)
        : values(first, last, bucket_count, hash, KeyEqual{}, alloc)
    {}

    string_map(std::initializer_list<value_type> init)
        : values(init)
    {}

    string_map(std::initializer_list<value_type> init,
               size_type                         bucket_count,
               const Hash&                       hash  = Hash{},
               const KeyEqual&                   equal = KeyEqual{},
               const allocator_type&             alloc = allocator_type{})
        : values(init, bucket_count, hash, equal, alloc)
    {}

    string_map(std::initializer_list<value_type> init,
               size_type                         bucket_count,
               const allocator_type&             alloc = allocator_type{})
        : values(init, bucket_count, Hash{}, KeyEqual{}, alloc)
    {}

    string_map(const string_map&) = default;
    string_map(const string_map& other, const allocator_type& alloc)
        : values(other.values, alloc)
    {}

    string_map(string_map&&) noexcept = default;
    string_map(string_map&& other, const allocator_type& alloc)
        : values(std::exchange(std::move(other).values, {}), alloc)
    {}

#if defined(__cpp_lib_containers_ranges) && __cpp_lib_containers_ranges >= 202202L
    template<container_compatible_range<value_type> R>
    string_map(std::from_range_t /*unused*/, R&& rg)
        : values(std::from_range_t{}, std::forward<R>(rg))
    {}

    template<container_compatible_range<value_type> R>
    string_map(std::from_range_t /*unused*/,
               R&&                   rg,
               size_type             bucket_count,
               const Hash&           hash  = Hash{},
               const KeyEqual&       equal = KeyEqual{},
               const allocator_type& alloc = allocator_type{})
        : values(std::from_range_t{}, std::forward<R>(rg), bucket_count, hash, equal, alloc)
    {}

    template<container_compatible_range<value_type> R>
    string_map(std::from_range_t /*unused*/,
               R&&                   rg,
               size_type             bucket_count,
               const allocator_type& alloc = allocator_type{})
        : values(std::from_range_t{}, std::forward<R>(rg), bucket_count, Hash{}, KeyEqual{}, alloc)
    {}

    template<container_compatible_range<value_type> R>
    string_map(std::from_range_t /*unused*/,
               R&&                   rg,
               size_type             bucket_count,
               const Hash&           hash,
               const allocator_type& alloc = allocator_type{})
        : values(std::from_range_t{}, std::forward<R>(rg), bucket_count, hash, KeyEqual{}, alloc)
    {}
#endif

    ~string_map() = default;

    string_map& operator=(const string_map& other)     = default;
    string_map& operator=(string_map&& other) noexcept = default;

    string_map& operator=(std::initializer_list<value_type> ilist) { values = ilist; }

    allocator_type get_allocator() const noexcept { return values.get_allocator(); }

    [[nodiscard]] iterator       begin() noexcept { return values.begin(); }
    [[nodiscard]] const_iterator begin() const noexcept { return values.begin(); }
    [[nodiscard]] const_iterator cbegin() const noexcept { return values.cbegin(); }

    [[nodiscard]] iterator       end() noexcept { return values.end(); }
    [[nodiscard]] const_iterator end() const noexcept { return values.end(); }
    [[nodiscard]] const_iterator cend() const noexcept { return values.cend(); }

    [[nodiscard]] bool      empty() const noexcept { return values.empty(); }
    [[nodiscard]] size_type size() const noexcept { return values.size(); }
    [[nodiscard]] size_type max_size() const noexcept { return values.max_size(); }

    void clear() noexcept { values.clear(); }

    std::pair<iterator, bool> insert(const value_type& value) { return values.insert(value); }
    std::pair<iterator, bool> insert(value_type&& value) { return values.insert(std::move(value)); }

    template<typename P>
    std::pair<iterator, bool> insert(P&& value)
    {
        return values.insert(std::forward<P>(value));
    }

    iterator insert(const_iterator hint, const value_type& value) { return values.insert(hint, value); }
    iterator insert(const_iterator hint, value_type&& value) { return values.insert(hint, std::move(value)); }

    template<typename P>
    iterator insert(const_iterator hint, P&& value)
    {
        return values.insert(hint, std::forward<P>(value));
    }

    template<typename InputIt>
    void insert(InputIt first, InputIt last)
    {
        return values.insert(first, last);
    }

    void               insert(std::initializer_list<value_type> ilist) { values.insert(ilist); }
    insert_return_type insert(node_type&& nh) { return values.insert(std::move(nh)); }
    iterator           insert(const_iterator hint, node_type&& nh) { return values.insert(hint, std::move(nh)); }

#if defined(__cpp_lib_containers_ranges) && __cpp_lib_containers_ranges >= 202202L
    template<container_compatible_range<value_type> R>
    void insert_range(R&& rg)
    {
        values.insert_range(std::forward<R>(rg));
    }
#endif

    template<typename M>
    std::pair<iterator, bool> insert_or_assign(const String& k, M&& obj)
    {
        return values.insert_or_assign(k, std::forward<M>(obj));
    }

    template<typename M>
    std::pair<iterator, bool> insert_or_assign(String&& k, M&& obj)
    {
        return values.insert_or_assign(std::move(k), std::forward<M>(obj));
    }

    template<typename M>
    std::pair<iterator, bool> insert_or_assign(StringView k, M&& obj)
    {
        return values.insert_or_assign(k, std::forward<M>(obj));
    }

    template<typename M>
    iterator insert_or_assign(const_iterator hint, const String& k, M&& obj)
    {
        return values.insert_or_assign(hint, k, std::forward<M>(obj));
    }

    template<typename M>
    iterator insert_or_assign(const_iterator hint, String&& k, M&& obj)
    {
        return values.insert_or_assign(hint, std::move(k), std::forward<M>(obj));
    }

    template<typename M>
    iterator insert_or_assign(const_iterator hint, StringView k, M&& obj)
    {
        return values.insert_or_assign(hint, k, std::forward<M>(obj));
    }

    template<typename... Args>
    std::pair<iterator, bool> emplace(Args&&... args)
    {
        return values.emplace(std::forward<Args>(args)...);
    }

    template<typename... Args>
    std::pair<iterator, bool> emplace_hint(const_iterator hint, Args&&... args)
    {
        return values.emplace_hint(hint, std::forward<Args>(args)...);
    }

    template<typename... Args>
    std::pair<iterator, bool> try_emplace(const String& k, Args&&... args)
    {
        return values.try_emplace(k, std::forward<Args>(args)...);
    }

    template<typename... Args>
    std::pair<iterator, bool> try_emplace(String&& k, Args&&... args)
    {
        return values.try_emplace(std::move(k), std::forward<Args>(args)...);
    }

    template<typename... Args>
    std::pair<iterator, bool> try_emplace(StringView k, Args&&... args)
    {
        return values.try_emplace(k, std::forward<Args>(args)...);
    }

    template<typename... Args>
    std::pair<iterator, bool> try_emplace(const_iterator hint, const String& k, Args&&... args)
    {
        return values.try_emplace(hint, k, std::forward<Args>(args)...);
    }

    template<typename... Args>
    std::pair<iterator, bool> try_emplace(const_iterator hint, String&& k, Args&&... args)
    {
        return values.try_emplace(hint, std::move(k), std::forward<Args>(args)...);
    }

    template<typename... Args>
    std::pair<iterator, bool> try_emplace(const_iterator hint, StringView k, Args&&... args)
    {
        return values.try_emplace(hint, k, std::forward<Args>(args)...);
    }

    iterator  erase(iterator pos) { return values.erase(pos); }
    iterator  erase(const_iterator pos) { return values.erase(pos); }
    iterator  erase(const_iterator first, const_iterator last) { return values.erase(first, last); }
    size_type erase(const String& key) { return values.erase(key); }
    size_type erase(StringView key) { return values.erase(key); }

#if defined(__cpp_lib_associative_heterogeneous_erasure) && __cpp_lib_associative_heterogeneous_erasure >= 202110L
    template<typename K>
    inline size_type erase(K&& x)
    {
        return values.erase(std::forward<K>(x));
    }
#endif

    void swap(string_map& other) noexcept(
        // clang-format off
        std::allocator_traits<allocator_type>::is_always_equal::value
        && std::is_nothrow_swappable_v<Hash>
        && std::is_nothrow_swappable_v<KeyEqual>
        // clang-format on
    )
    {
        values.swap(other.values);
    }

    node_type extract(const_iterator position) { return values.extract(position); }
    node_type extract(const String& k) { return values.extract(k); }
    node_type extract(StringView k) { return values.extract(k); }

#if defined(__cpp_lib_associative_heterogeneous_erasure) && __cpp_lib_associative_heterogeneous_erasure >= 202110L
    template<typename K>
    inline node_type extract(K&& x)
    {
        return values.extract(std::forward<K>(x));
    }
#endif

    template<typename H2, typename P2>
    void merge(string_map& source)
    {
        values.merge(source.values);
    }

    template<typename H2, typename P2>
    void merge(string_map&& source)
    {
        values.merge(std::move(source).values);
    }

    mapped_type&       at(const String& key) { return values.at(key); }
    const mapped_type& at(const String& key) const { return values.at(key); }
    mapped_type&       at(StringView key) { return values.at(key); }
    const mapped_type& at(StringView key) const { return values.at(key); }

    mapped_type& operator[](const String& key) { return values[key]; }

    mapped_type& operator[](StringView key)
    {
        auto iter = values.find(key);
        if (iter == values.end())
        {
            auto [it, inserted] = values.emplace(String(key), mapped_type{});
            return it->second;
        }

        return iter->second;
    }

    size_type count(const String& key) const { return values.count(key); }
    size_type count(StringView key) const { return values.count(key); }

#if defined(__cpp_lib_generic_unordered_lookup) && __cpp_lib_generic_unordered_lookup >= 201811L
    template<typename K>
    size_type count(const K& x) const
    {
        return values.count(x);
    }
#endif

    iterator       find(const String& key) { return values.find(key); }
    const_iterator find(const String& key) const { return values.find(key); }
    iterator       find(StringView key) { return values.find(key); }
    const_iterator find(StringView key) const { return values.find(key); }

#if defined(__cpp_lib_generic_unordered_lookup) && __cpp_lib_generic_unordered_lookup >= 201811L
    template<typename K>
    iterator find(const K& x)
    {
        return values.find(x);
    }

    template<typename K>
    const_iterator find(const K& x) const
    {
        return values.find(x);
    }
#endif

    [[nodiscard]] bool contains(const String& key) const { return values.contains(key); }
    [[nodiscard]] bool contains(StringView key) const { return values.contains(key); }

#if defined(__cpp_lib_generic_unordered_lookup) && __cpp_lib_generic_unordered_lookup >= 201811L
    template<typename K>
    [[nodiscard]] bool contains(const K& x) const
    {
        return values.contains(x);
    }
#endif

    std::pair<iterator, iterator>             equal_range(const String& key) { return values.equal_range(key); }
    std::pair<const_iterator, const_iterator> equal_range(const String& key) const { return values.equal_range(key); }
    std::pair<iterator, iterator>             equal_range(StringView key) { return values.equal_range(key); }
    std::pair<const_iterator, const_iterator> equal_range(StringView key) const { return values.equal_range(key); }

#if defined(__cpp_lib_generic_unordered_lookup) && __cpp_lib_generic_unordered_lookup >= 201811L
    template<typename K>
    std::pair<iterator, iterator> equal_range(const K& x)
    {
        return values.equal_range(x);
    }

    template<typename K>
    std::pair<const_iterator, const_iterator> equal_range(const K& x) const
    {
        return values.equal_range(x);
    }
#endif

    local_iterator       begin(size_type n) { return values.begin(n); }
    const_local_iterator begin(size_type n) const { return values.begin(n); }
    const_local_iterator cbegin(size_type n) const { return values.begin(n); }

    local_iterator       end(size_type n) { return values.end(n); }
    const_local_iterator end(size_type n) const { return values.end(n); }
    const_local_iterator cend(size_type n) const { return values.end(n); }

    size_type bucket_count() const { return values.bucket_count(); }
    size_type max_bucket_count() const { return values.max_bucket_count(); }
    size_type bucket_size(size_type n) const { return values.bucket_size(n); }

    size_type bucket(const String& key) const { return values.bucket(key); }
    size_type bucket(StringView key) const { return values.bucket(key); }

    [[nodiscard]] float load_factor() const { return values.load_factor(); }

    [[nodiscard]] float max_load_factor() const { return values.max_load_factor(); }
    void                max_load_factor(float ml) { values.max_load_factor(ml); }

    void rehash(size_type count) { values.rehash(count); }
    void reserve(size_type count) { values.reserve(count); }

    Hash     hash_function() const { return values.hash_function(); }
    KeyEqual key_eq() const { return values.key_eq(); }

    friend bool operator==(const string_map& lhs, const string_map& rhs) noexcept { return lhs.values == rhs.values; }
    friend bool operator!=(const string_map& lhs, const string_map& rhs) noexcept { return lhs.values != rhs.values; }

    template<typename Pred>
    friend size_type erase_if(string_map& c, Pred pred)
    {
        return std::erase_if(c.values, pred);
    }

private:
    map_type values;
};

}
