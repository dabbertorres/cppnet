#include "util/uuid.hpp"

#include <chrono>
#include <cstddef>
#include <random>
#include <span>
#include <stdexcept>
#include <string_view>

#include "encoding/hex.hpp"

namespace net::util
{

uuid uuid::generate_v4()
{
    std::random_device rd;

    uuid  out{};
    auto* ptr = reinterpret_cast<std::random_device::result_type*>(out.data.data());

    for (auto i = 0u; i < out.data.size() / sizeof(std::random_device::result_type); ++i)
    {
        (*ptr++) = rd();
    }

    out.set_version(4);
    out.set_variant(8);

    return out;
}

uuid uuid::generate_v7()
{
    using clock = std::chrono::steady_clock;

    /*static std::atomic<clock::duration> last_time;*/

    auto now    = clock::now().time_since_epoch();
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now);
    auto seq    = now - now_ms;

    uuid out{};
    out.data[0] = std::byte(now_ms.count() >> 40);
    out.data[1] = std::byte(now_ms.count() >> 32);
    out.data[2] = std::byte(now_ms.count() >> 24);
    out.data[3] = std::byte(now_ms.count() >> 16);
    out.data[4] = std::byte(now_ms.count() >> 8);
    out.data[5] = std::byte(now_ms.count() >> 0);

    out.data[6] = std::byte((seq.count() >> 8) & 0x0f);
    out.data[7] = std::byte(seq.count());

    std::random_device rd;

    auto* ptr = reinterpret_cast<std::random_device::result_type*>(out.data.data() + 8);

    for (auto i = 8 / sizeof(std::random_device::result_type);
         i < out.data.size() / sizeof(std::random_device::result_type);
         ++i)
    {
        (*ptr++) = rd();
    }

    out.set_version(7);
    out.set_variant(8);

    return out;
}

uuid uuid::nil() { return {{}}; }

uuid uuid::max()
{
    return {
        {
         std::byte{0xff},
         std::byte{0xff},
         std::byte{0xff},
         std::byte{0xff},
         std::byte{0xff},
         std::byte{0xff},
         std::byte{0xff},
         std::byte{0xff},
         std::byte{0xff},
         std::byte{0xff},
         std::byte{0xff},
         std::byte{0xff},
         std::byte{0xff},
         std::byte{0xff},
         std::byte{0xff},
         std::byte{0xff},
         }
    };
}

uuid uuid_parse(std::string_view sv)
{
    using namespace std::string_view_literals;

    uuid out{};

    switch (sv.length())
    {
    case 36: // xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
        break;

    case 36 + 9: // urn:uuid:xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
        if (!sv.starts_with("urn:uuid:")) throw std::invalid_argument{"expected 'urn:uuid:' prefix"};

        sv.remove_prefix("urn:uuid:"sv.length());
        break;

    case 36 + 2: // {xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}
        if (!sv.starts_with("{") || !sv.ends_with("}")) throw std::invalid_argument{"expected wrapped in '{}'"};

        sv = sv.substr(1, sv.length() - 2);
        break;

    case 32: // xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx
        if (!encoding::hex::decode_to(sv, out.data)) throw std::invalid_argument{"invalid hexadecimal"};

        // don't continue, since this format doesn't match the others
        return out;

    default: throw std::invalid_argument{"unrecognized format"};
    }

    auto out_dest = std::span{out.data};

    if (!encoding::hex::decode_to(sv.substr(0, 8), out_dest.subspan(0, 4)))
        throw std::invalid_argument{"invalid hexadecimal in group 0"};

    if (sv[8] != '-') throw std::invalid_argument{"expected '-' after group 0"};

    if (!encoding::hex::decode_to(sv.substr(9, 4), out_dest.subspan(4, 2)))
        throw std::invalid_argument{"invalid hexadecimal in group 1"};

    if (sv[13] != '-') throw std::invalid_argument{"expected '-' after group 1"};

    if (!encoding::hex::decode_to(sv.substr(14, 4), out_dest.subspan(6, 2)))
        throw std::invalid_argument{"invalid hexadecimal in group 2"};

    if (sv[18] != '-') throw std::invalid_argument{"expected '-' after group 2"};

    if (!encoding::hex::decode_to(sv.substr(19, 4), out_dest.subspan(8, 2)))
        throw std::invalid_argument{"invalid hexadecimal in group 3"};

    if (sv[23] != '-') throw std::invalid_argument{"expected '-' after group 3"};

    if (!encoding::hex::decode_to(sv.substr(24), out_dest.subspan(10, 6)))
        throw std::invalid_argument{"invalid hexadecimal in group 4"};

    return out;
}

}
