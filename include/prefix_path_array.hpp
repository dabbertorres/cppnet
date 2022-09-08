#pragma once

#include <memory>
#include <string>

namespace net::util
{

template<typename V,
         typename CharT     = char,
         typename Traits    = std::char_traits<CharT>,
         typename Allocator = std::allocator<CharT>>
class prefix_path_array
{
public:
    using string = std::basic_string<CharT, Traits, Allocator>;

    prefix_path_array()                             = default;
    prefix_path_array(const prefix_path_array&)     = default;
    prefix_path_array(prefix_path_array&&) noexcept = default;

    prefix_path_array& operator=(const prefix_path_array&)     = default;
    prefix_path_array& operator=(prefix_path_array&&) noexcept = default;

    ~prefix_path_array() = default;

    void insert(string path, V value) {}

    V* lookup(string path) const {}

private:
    struct elem
    {
        string tail;
        V      value;
    };

    std::vector<string> prefices;
    std::vector<elem>   values;
};

}
