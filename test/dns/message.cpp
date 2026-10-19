#include "dns/message.hpp"

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "dns/error.hpp"
#include "encoding/hex.hpp"
#include "ipv4_addr.hpp"
#include "ipv6_addr.hpp"

namespace dns = net::dns;

namespace
{

std::string to_hex(std::span<const std::byte> bytes)
{
    auto hex = net::encoding::hex::encode(bytes);
    for (auto& c : hex) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return hex;
}

// wire builds a message's bytes by hand.
struct wire
{
    std::vector<std::byte> bytes;

    [[nodiscard]] std::size_t size() const noexcept { return bytes.size(); }

    wire& u8(unsigned value)
    {
        bytes.push_back(static_cast<std::byte>(value));
        return *this;
    }

    wire& u16(unsigned value) { return u8((value >> 8U) & 0xffU).u8(value & 0xffU); }
    wire& u32(std::uint32_t value) { return u16(value >> 16U).u16(value & 0xffffU); }

    wire& label(std::string_view text)
    {
        u8(static_cast<unsigned>(text.size()));
        for (const auto c : text) u8(static_cast<unsigned char>(c));
        return *this;
    }

    // name appends text's labels, and the root, uncompressed.
    wire& name(std::string_view text)
    {
        while (!text.empty())
        {
            const auto dot = text.find('.');
            label(text.substr(0, dot));
            text.remove_prefix(dot == std::string_view::npos ? text.size() : dot + 1);
        }
        return u8(0);
    }

    wire& pointer(std::size_t offset) { return u16(0xc000U | static_cast<unsigned>(offset)); }

    // header appends a header with flags (the third and fourth octets) and the sections' counts.
    wire&
    header(unsigned id, unsigned flags, unsigned questions, unsigned answers, unsigned authority, unsigned additional)
    {
        return u16(id).u16(flags).u16(questions).u16(answers).u16(authority).u16(additional);
    }

    // record_head appends a record's fields after its name, up to its RDLENGTH.
    wire& record_head(dns::type type, std::uint32_t ttl, unsigned length)
    {
        return u16(static_cast<unsigned>(type)).u16(1).u32(ttl).u16(length);
    }

    // opt appends an OPT record (with no options).
    wire& opt(unsigned udp_size, std::uint32_t ttl) { return u8(0).u16(41).u16(udp_size).u32(ttl).u16(0); }
};

// a_response is a response of one question (example.com's A records), whose answer section starts after it.
wire a_response(unsigned answers, unsigned additional = 0, unsigned flags = 0x8180)
{
    wire w;
    w.header(0x1234, flags, 1, answers, 0, additional).name("example.com").u16(1).u16(1);
    return w;
}

void require_bad(const wire& w)
{
    const auto decoded = dns::decode(w.bytes);
    REQUIRE_FALSE(decoded.has_value());
    REQUIRE(decoded.error() == dns::errc::bad_response);
}

}

TEST_CASE("a query is encoded with an OPT record offering 1232 bytes", "[dns][message][encode]")
{
    std::vector<std::byte> out;
    REQUIRE_FALSE(dns::encode(dns::make_query(0xbeef, "www.example.com", dns::type::aaaa), out));

    // the header (ID, RD, one question, one additional record), the question (AAAA, IN), and the OPT record (the root,
    // type 41, 1232 bytes, extended RCODE and version 0, no flags, no options).
    REQUIRE(to_hex(out)
            == "beef0100"
               "000100000000"
               "0001"
               "03777777076578616d706c6503636f6d00"
               "001c0001"
               "00"
               "0029"
               "04d0"
               "00000000"
               "0000");

    SECTION("and without one, if asked")
    {
        out.clear();
        REQUIRE_FALSE(dns::encode(dns::make_query(0x0001, "example.com.", dns::type::a, false), out));
        REQUIRE(to_hex(out) == "000101000001000000000000076578616d706c6503636f6d0000010001");
    }
}

TEST_CASE("the header is encoded field by field", "[dns][message][encode]")
{
    struct test_case
    {
        std::string_view                  name;
        std::function<void(dns::header&)> set;
        std::string_view                  flags; // the third and fourth octets
    };

    const auto tc = GENERATE(test_case{"QR", [](dns::header& h) { h.response = true; }, "8000"},
                             test_case{"OPCODE", [](dns::header& h) { h.opcode = dns::opcode{15}; }, "7800"},
                             test_case{"AA", [](dns::header& h) { h.authoritative = true; }, "0400"},
                             test_case{"TC", [](dns::header& h) { h.truncated = true; }, "0200"},
                             test_case{"RD", [](dns::header& h) { h.recursion_desired = true; }, "0100"},
                             test_case{"RA", [](dns::header& h) { h.recursion_available = true; }, "0080"},
                             test_case{"AD", [](dns::header& h) { h.authentic_data = true; }, "0020"},
                             test_case{"RCODE", [](dns::header& h) { h.rcode = dns::rcode{15}; }, "000f"});

    CAPTURE(tc.name);

    dns::message msg;
    msg.header.id = 0xabcd;
    tc.set(msg.header);

    std::vector<std::byte> out;
    REQUIRE_FALSE(dns::encode(msg, out));
    REQUIRE(to_hex(out) == "abcd" + std::string{tc.flags} + "0000000000000000");

    const auto decoded = dns::decode(out);
    REQUIRE(decoded.has_value());
    REQUIRE(*decoded == msg);
}

TEST_CASE("a response's A, AAAA and CNAME records are decoded, and others skipped", "[dns][message][decode]")
{
    // www.example.com is an alias of cdn.example.com, which has an A, an AAAA and a TXT record; and an OPT record.
    wire w;
    w.header(0xbeef, 0x8180, 1, 4, 0, 1);
    REQUIRE(w.size() == 12);
    w.name("www.example.com").u16(1).u16(1); // example.com is at 16
    REQUIRE(w.size() == 33);
    w.pointer(12).record_head(dns::type::cname, 3600, 6).label("cdn").pointer(16); // cdn.example.com is at 45
    REQUIRE(w.size() == 51);
    w.pointer(45).record_head(dns::type::a, 60, 4).u8(93).u8(184).u8(215).u8(14);
    w.pointer(45)
        .record_head(dns::type::aaaa, 60, 16)
        .u32(0x2606'2800)
        .u32(0x021f'cb07)
        .u32(0x6820'80da)
        .u32(0xaf6b'8b2c);
    w.pointer(45).record_head(dns::type{16}, 60, 6).label("hello");
    w.opt(4096, 0);

    const auto decoded = dns::decode(w.bytes);
    REQUIRE(decoded.has_value());

    const auto& msg = *decoded;
    CHECK(msg.header
          == dns::header{.id = 0xbeef, .response = true, .recursion_desired = true, .recursion_available = true});
    CHECK(msg.questions
          == std::vector<dns::question>{
              {.name = "www.example.com", .type = dns::type::a}
    });
    CHECK(msg.answers
          == std::vector<dns::record>{
              {.name = "www.example.com",.ttl = 3600,                                      .data = dns::cname{"cdn.example.com"}                                                      },
              {.name = "cdn.example.com",   .ttl = 60,                                                .data = net::ipv4_addr{93, 184, 215, 14}},
              {.name = "cdn.example.com",
               .ttl  = 60,
               .data = net::ipv6_addr{0x2606, 0x2800, 0x021f, 0xcb07, 0x6820, 0x80da, 0xaf6b, 0x8b2c}                                         },
    });
    CHECK(msg.answers[0].type() == dns::type::cname);
    CHECK(msg.answers[1].type() == dns::type::a);
    CHECK(msg.answers[2].type() == dns::type::aaaa);
    CHECK(msg.authorities.empty());
    CHECK(msg.additionals.empty());
    CHECK(msg.edns == dns::edns{.udp_size = 4096});

    SECTION("cut short anywhere, it's rejected")
    {
        for (std::size_t length = 0; length < w.size(); ++length)
        {
            CAPTURE(length);
            const auto prefix = dns::decode(std::span{w.bytes}.first(length));
            REQUIRE_FALSE(prefix.has_value());
            REQUIRE(prefix.error() == dns::errc::bad_response);
        }
    }

    SECTION("what follows its last record is ignored")
    {
        w.u8(0).u8(0);
        const auto longer = dns::decode(w.bytes);
        REQUIRE(longer.has_value());
        REQUIRE(*longer == msg);
    }
}

TEST_CASE("RFC 1035's example of compression decodes", "[dns][message][compression]")
{
    // section 4.1.4: F.ISI.ARPA, then FOO.F.ISI.ARPA and ARPA pointing into it, and the root; here as questions, the
    // first at 12 rather than 20.
    wire w;
    w.header(0, 0, 4, 0, 0, 0);
    w.name("F.ISI.ARPA").u16(1).u16(1);
    w.label("FOO").pointer(12).u16(1).u16(1);
    w.pointer(18).u16(1).u16(1);
    w.u8(0).u16(1).u16(1);

    const auto decoded = dns::decode(w.bytes);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->questions.size() == 4);
    CHECK(decoded->questions[0].name == "F.ISI.ARPA");
    CHECK(decoded->questions[1].name == "FOO.F.ISI.ARPA");
    CHECK(decoded->questions[2].name == "ARPA");
    CHECK(decoded->questions[3].name.empty());
}

TEST_CASE("compression pointers only point backwards", "[dns][message][compression]")
{
    SECTION("forwards")
    {
        // a question pointing at the name after it.
        wire w;
        w.header(0, 0, 2, 0, 0, 0).pointer(18).u16(1).u16(1).name("a").u16(1).u16(1);
        require_bad(w);
    }

    SECTION("at itself")
    {
        wire w;
        w.header(0, 0, 1, 0, 0, 0).pointer(12).u16(1).u16(1);
        require_bad(w);
    }

    SECTION("at a label before it, in the same name: a loop")
    {
        wire w;
        w.header(0, 0, 1, 0, 0, 0).label("a").pointer(12).u16(1).u16(1);
        require_bad(w);
    }

    SECTION("at a pointer that points forwards again: a loop")
    {
        // a skipped record's data (at 28) points at the next record's name (at 30), which points back at it.
        wire w;
        w.header(0, 0, 1, 2, 0, 0).u8(0).u16(1).u16(1);
        w.u8(0).record_head(dns::type{16}, 0, 2);
        REQUIRE(w.size() == 28);
        w.pointer(30);
        w.pointer(28).record_head(dns::type::a, 0, 4).u32(0x7f00'0001);
        require_bad(w);
    }
}

TEST_CASE("a name follows at most max_pointers compression pointers", "[dns][message][compression][limits]")
{
    // a chain of pointers in a skipped record's data, each to the one before (the first to the question's root), and an
    // A record named by a pointer to the last: 1 more than the chain.
    const auto chained = [](int chain)
    {
        wire w;
        w.header(0, 0x8000, 1, 2, 0, 0).u8(0).u16(1).u16(1);
        w.u8(0).record_head(dns::type{16}, 0, 2 * static_cast<unsigned>(chain));
        REQUIRE(w.size() == 28);
        w.pointer(12);
        for (int i = 1; i < chain; ++i) w.pointer(28 + (2 * static_cast<std::size_t>(i - 1)));
        w.pointer(28 + (2 * static_cast<std::size_t>(chain - 1))).record_head(dns::type::a, 0, 4).u32(0x7f00'0001);
        return w;
    };

    const auto at_most = dns::decode(chained(dns::max_pointers - 1).bytes);
    REQUIRE(at_most.has_value());
    REQUIRE(at_most->answers.size() == 1);
    REQUIRE(at_most->answers[0].name.empty());

    require_bad(chained(dns::max_pointers));
}

TEST_CASE("names are at most 255 bytes, and labels 63", "[dns][message][limits]")
{
    const std::string l63(63, 'a');
    const std::string l62(62, 'b');
    const std::string l61(61, 'c');

    SECTION("uncompressed")
    {
        // three labels of 63 and one of 61 come to 255 with their length octets and the root's.
        wire ok = a_response(0);
        ok.bytes.resize(12);
        ok.label(l63).label(l63).label(l63).label(l61).u8(0).u16(1).u16(1);

        const auto decoded = dns::decode(ok.bytes);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->questions.at(0).name == std::format("{}.{}.{}.{}", l63, l63, l63, l61));

        wire over = a_response(0);
        over.bytes.resize(12);
        over.label(l63).label(l63).label(l63).label(l62).u8(0).u16(1).u16(1);
        require_bad(over);
    }

    SECTION("with a pointer")
    {
        // the question is three labels of 63 (193 bytes); a record's name adds one before them.
        const auto with = [&](const std::string& first)
        {
            wire w;
            w.header(0, 0x8000, 1, 1, 0, 0).label(l63).label(l63).label(l63).u8(0).u16(1).u16(1);
            w.label(first).pointer(12).record_head(dns::type::a, 0, 4).u32(0x7f00'0001);
            return w;
        };

        REQUIRE(dns::decode(with(l61).bytes).has_value());
        require_bad(with(l62));
    }

    SECTION("a label's length octet over 63 is a reserved kind of label")
    {
        for (const unsigned length : {0x40U, 0x80U})
        {
            CAPTURE(length);
            wire w;
            w.header(0, 0, 1, 0, 0, 0).u8(length);
            for (unsigned i = 0; i < 64; ++i) w.u8('a');
            w.u8(0).u16(1).u16(1);
            require_bad(w);
        }
    }

    SECTION("a label with a dot in it")
    {
        wire w;
        w.header(0, 0, 1, 0, 0, 0).label("a.b").u8(0).u16(1).u16(1);
        require_bad(w);
    }
}

TEST_CASE("records' data must be what their type and RDLENGTH say", "[dns][message][decode]")
{
    SECTION("RDLENGTH past the message")
    {
        wire w = a_response(1);
        w.pointer(12).record_head(dns::type::a, 0, 5).u32(0x7f00'0001);
        require_bad(w);
    }

    SECTION("counts past the data")
    {
        wire w = a_response(0xffff);
        w.pointer(12).record_head(dns::type::a, 0, 4).u32(0x7f00'0001);
        require_bad(w);
    }

    SECTION("an A record of another size")
    {
        wire w = a_response(1);
        w.pointer(12).record_head(dns::type::a, 0, 5).u32(0x7f00'0001).u8(0);
        require_bad(w);
    }

    SECTION("an AAAA record of another size")
    {
        wire w = a_response(1);
        w.pointer(12).record_head(dns::type::aaaa, 0, 4).u32(0x7f00'0001);
        require_bad(w);
    }

    SECTION("a CNAME record with more than its name")
    {
        wire w = a_response(1);
        w.pointer(12).record_head(dns::type::cname, 0, 3).pointer(12).u8(0);
        require_bad(w);
    }

    SECTION("a CNAME record whose name goes past its data")
    {
        wire w = a_response(2);
        w.pointer(12).record_head(dns::type::cname, 0, 2).label("a").u8(0);
        w.pointer(12).record_head(dns::type::a, 0, 4).u32(0x7f00'0001);
        require_bad(w);
    }

    SECTION("two OPT records")
    {
        wire w = a_response(0, 2);
        w.opt(1232, 0).opt(1232, 0);
        require_bad(w);
    }

    SECTION("an OPT record outside the additional section is skipped")
    {
        wire w = a_response(1);
        w.opt(1232, 0);
        const auto decoded = dns::decode(w.bytes);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->answers.empty());
        REQUIRE_FALSE(decoded->edns.has_value());
    }
}

TEST_CASE("the TC bit is reported", "[dns][message][decode]")
{
    const auto truncated = GENERATE(false, true);

    const wire w       = a_response(0, 0, truncated ? 0x8380 : 0x8180);
    const auto decoded = dns::decode(w.bytes);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->header.truncated == truncated);
}

TEST_CASE("RCODEs are mapped to errors", "[dns][message][rcode]")
{
    struct test_case
    {
        unsigned        rcode;   // the header's 4 bits
        std::uint32_t   opt_ttl; // an OPT record's TTL (its extended RCODE the top 8 bits), if not 0
        dns::rcode      decoded;
        std::error_code error;
    };

    const auto tc = GENERATE(test_case{0, 0, dns::rcode::no_error, {}},
                             test_case{1, 0, dns::rcode::format_error, dns::errc::server_failure},
                             test_case{2, 0, dns::rcode::server_failure, dns::errc::server_failure},
                             test_case{3, 0, dns::rcode::name_error, dns::errc::name_not_found},
                             test_case{4, 0, dns::rcode::not_implemented, dns::errc::server_failure},
                             test_case{5, 0, dns::rcode::refused, dns::errc::refused},
                             test_case{9, 0, dns::rcode{9}, dns::errc::server_failure},
                             test_case{0, 0x0100'0000, dns::rcode::bad_version, dns::errc::server_failure},
                             test_case{3, 0x0200'0000, dns::rcode{0x23}, dns::errc::server_failure});

    CAPTURE(tc.rcode, tc.opt_ttl);

    wire w = a_response(0, tc.opt_ttl != 0 ? 1 : 0, 0x8180 | tc.rcode);
    if (tc.opt_ttl != 0) w.opt(1232, tc.opt_ttl);

    const auto decoded = dns::decode(w.bytes);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->header.rcode == tc.decoded);
    REQUIRE(dns::rcode_error(decoded->header.rcode) == tc.error);

    // and the extended RCODE is split again.
    std::vector<std::byte> out;
    REQUIRE_FALSE(dns::encode(*decoded, out));
    REQUIRE(out == w.bytes);
}

TEST_CASE("messages round-trip", "[dns][message][encode]")
{
    dns::message msg;
    msg.header    = {.id = 7, .response = true, .authoritative = true, .rcode = dns::rcode::bad_version};
    msg.questions = {
        {.name = "example.com", .type = dns::type::aaaa}
    };
    msg.answers = {
        {  .name = "example.com", .ttl = 1, .data = dns::cname{"a.example.net"}},
        {.name = "a.example.net", .ttl = 2,  .data = net::ipv6_addr::loopback()},
    };
    msg.authorities = {
        {.name = "", .rclass = dns::rclass{3}, .ttl = 3, .data = net::ipv4_addr::loopback()}
    };
    msg.additionals = {
        {.name = "b.example.net", .ttl = 4, .data = net::ipv4_addr{10, 0, 0, 1}}
    };
    msg.edns = dns::edns{.udp_size = 512, .version = 1, .dnssec_ok = true};

    std::vector<std::byte> out;
    REQUIRE_FALSE(dns::encode(msg, out));

    const auto decoded = dns::decode(out);
    REQUIRE(decoded.has_value());
    REQUIRE(*decoded == msg);
}

TEST_CASE("encoding rejects what can't be encoded, leaving the output as it was", "[dns][message][encode]")
{
    struct test_case
    {
        std::string_view                   name;
        std::function<void(dns::message&)> set;
    };

    const auto tc = GENERATE(
        test_case{"a label over 63", [](dns::message& m) { m.questions[0].name = std::string(64, 'a') + ".com"; }},
        test_case{"a name over 255",
                  [](dns::message& m)
                  {
                      const std::string l63(63, 'a');
                      m.questions[0].name = std::format("{}.{}.{}.{}", l63, l63, l63, std::string(62, 'b'));
                  }},
        test_case{"an empty label", [](dns::message& m) { m.questions[0].name = "a..b"; }},
        test_case{"an empty first label", [](dns::message& m) { m.questions[0].name = ".a"; }},
        test_case{"two final dots", [](dns::message& m) { m.questions[0].name = "a.."; }},
        test_case{"just two dots", [](dns::message& m) { m.questions[0].name = ".."; }},
        test_case{"a CNAME's target",
                  [](dns::message& m) { m.answers.push_back({.name = "a", .data = dns::cname{"a..b"}}); }},
        test_case{"an opcode over 15", [](dns::message& m) { m.header.opcode = dns::opcode{16}; }},
        test_case{"an RCODE over 15 without EDNS(0)",
                  [](dns::message& m)
                  {
                      m.edns.reset();
                      m.header.rcode = dns::rcode::bad_version;
                  }},
        test_case{"an RCODE over 4095", [](dns::message& m) { m.header.rcode = dns::rcode{0x1000}; }});

    CAPTURE(tc.name);

    auto msg = dns::make_query(1, "example.com", dns::type::a);
    tc.set(msg);

    std::vector<std::byte> out{std::byte{1}, std::byte{2}};
    REQUIRE(dns::encode(msg, out) == std::errc::invalid_argument);
    REQUIRE(out == std::vector{std::byte{1}, std::byte{2}});
}

TEST_CASE("names' final dots are dropped, and the root is empty", "[dns][message][encode]")
{
    const auto [name, decoded_name] = GENERATE(std::pair{"example.com.", "example.com"},
                                               std::pair{"example.com", "example.com"},
                                               std::pair{".", ""},
                                               std::pair{"", ""});

    CAPTURE(name);

    std::vector<std::byte> out;
    REQUIRE_FALSE(dns::encode(dns::make_query(1, name, dns::type::a), out));

    const auto decoded = dns::decode(out);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->questions.at(0).name == decoded_name);
}
