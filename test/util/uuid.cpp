#include "util/uuid.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

#include <catch.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

TEST_CASE("version", "[util][uuid][version]")
{
    struct test_case
    {
        std::string     name;
        net::util::uuid input;
        std::uint8_t    expect;
    };

    auto tc = GENERATE(
        test_case{
            .name   = "max version is 0xf",
            .input  = net::util::uuid::max(),
            .expect = 0xf,
        },
        test_case{
            .name   = "nil version is 0x0",
            .input  = net::util::uuid::nil(),
            .expect = 0x0,
        });

    SECTION(tc.name)
    {
        CHECK(tc.input.version() == tc.expect);
    }
}

/*TEST_CASE("variant", "[util][uuid][variant]")*/
/*{*/
/*    std::array<net::util::uuid, 6> variants{};*/
/*    variants[0].data[8] = std::byte{0x8F};*/
/*    variants[1].data[8] = std::byte{0x9F};*/
/*    variants[2].data[8] = std::byte{0xAF};*/
/*    variants[3].data[8] = std::byte{0xBF};*/
/*    variants[4].data[8] = std::byte{0xCF};*/
/*    variants[5].data[8] = std::byte{0xDF};*/
/**/
/*    for (auto i = 0u; i < variants.size(); ++i)*/
/*    {*/
/*        REQUIRE(static_cast<std::uint32_t>(variants[i].variant()) == i);*/
/*    }*/
/*}*/

TEST_CASE("parsing", "[util][uuid][uuid_parse]")
{
    SECTION("standard format")
    {
        auto actual = net::util::uuid_parse("8D8AC610-566D-4EF0-9C22-186B2A5ED793");

        net::util::uuid expect{
            {
             std::byte(0x8d),
             std::byte(0x8a),
             std::byte(0xc6),
             std::byte(0x10),
             std::byte(0x56),
             std::byte(0x6d),
             std::byte(0x4e),
             std::byte(0xf0),
             std::byte(0x9c),
             std::byte(0x22),
             std::byte(0x18),
             std::byte(0x6b),
             std::byte(0x2a),
             std::byte(0x5e),
             std::byte(0xd7),
             std::byte(0x93),
             }
        };

        REQUIRE(expect == actual);
    }
}
