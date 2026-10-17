#!/usr/bin/env python3
"""Generates HPACK's static table and Huffman code (src/http/hpack.cpp) from RFC 7541's text.

    curl -sSfL https://www.rfc-editor.org/rfc/rfc7541.txt -o rfc7541.txt
    python3 tools/hpack/gen_tables.py rfc7541.txt

It checks, as it parses, that the static table has entries 1 to 61, that each code's bits match its value and length,
that no two symbols share a code, and that the code is complete (its symbols' 2^-length sum to 1). The output is then
clang-formatted with the source.
"""

import re
import sys
from fractions import Fraction


def main(path):
    text = open(path, encoding="utf-8").read()
    a = text.index("Appendix A.  Static Table Definition")
    b = text.index("Appendix B.  Huffman Code")
    c = text.index("Appendix C.  Examples")

    static = []
    for line in text[a:b].splitlines():
        m = re.match(r"\s*\|\s*(\d+)\s*\|\s*(\S+)\s*\|\s*(.*?)\s*\|\s*$", line)
        if m:
            static.append((int(m.group(1)), m.group(2), m.group(3)))
    assert [i for i, _, _ in static] == list(range(1, 62)), static

    codes = {}
    for line in text[b:c].splitlines():
        m = re.match(r"\s*(?:'.'\s*)?\(\s*(\d+)\)\s*\|([01|]+)\s+([0-9a-f]+)\s+\[\s*(\d+)\]", line) or re.match(
            r"\s*EOS\s*\(\s*(256)\)\s*\|([01|]+)\s+([0-9a-f]+)\s+\[\s*(\d+)\]", line
        )
        if not m:
            continue
        symbol, bits, value, length = int(m.group(1)), m.group(2).replace("|", ""), int(m.group(3), 16), int(m.group(4))
        assert len(bits) == length and int(bits, 2) == value, (symbol, bits, value, length)
        assert symbol not in codes, symbol
        codes[symbol] = (value, length)

    assert sorted(codes) == list(range(257)), len(codes)
    assert len(set(codes.values())) == len(codes), "two symbols share a code"
    assert sum(Fraction(1, 2**length) for _, length in codes.values()) == 1, "the code isn't complete"

    out = [
        "// RFC 7541, Appendix A: the static table (1-based in the RFC, so index i is entry i - 1).",
        "constexpr std::array<std::pair<std::string_view, std::string_view>, 61> static_table{{",
    ]
    out += [f'    {{"{name}", "{value}"}},' for _, name, value in static]
    out += [
        "}};",
        "",
        "// RFC 7541, Appendix B: each symbol's code and its length in bits; 256 is EOS.",
        "constexpr std::array<huffman_code, 257> huffman_codes{{",
    ]
    out += [f"    {{0x{codes[s][0]:x}, {codes[s][1]}}}," for s in range(257)]
    out += ["}};"]
    print("\n".join(out))


if __name__ == "__main__":
    main(sys.argv[1])
