#include <catch2/catch_tostring.hpp>

#include "http/headers.hpp"

namespace Catch
{

template<>
struct StringMaker<net::http::headers>
{
    static std::string convert(const net::http::headers& headers)
    {
        std::string build;

        for (const auto& header : headers)
        {
            build += header.first;
            build += ':';

            for (const auto& value : header.second)
            {
                build += ' ';
                build += value;
            }

            build += '\n';
        }

        return build;
    }
};

}
