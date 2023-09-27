#pragma once

namespace net::util
{

template<typename... Callables>
struct overloaded : Callables...
{
    using Callables::operator()...;
};

template<typename... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

}
