#!/usr/bin/env python3
"""Generates HPACK's tests' examples (rfc_examples() in test/http/hpack.cpp) from RFC 7541's text (Appendix C).

    curl -sSfL https://www.rfc-editor.org/rfc/rfc7541.txt -o rfc7541.txt
    python3 tools/hpack/gen_examples.py rfc7541.txt

Each example is its encoded block (hex), the fields it decodes to, and the dynamic table after it (newest first) and
its size. It checks that each table entry's size is its name's and value's lengths and 32, as the RFC states it, and
that the table's size is their sum. The output is then clang-formatted with the test.
"""

import re
import sys

# The RFC's sequences: those in C.3 to C.6 are each decoded by one decoder, in order; C.5 and C.6 with a table of 256.
GROUPS = [
    ("C.2.1", "a literal with indexing", 4096, 0, 1),
    ("C.2.2", "a literal without indexing", 4096, 1, 2),
    ("C.2.3", "a literal never indexed", 4096, 2, 3),
    ("C.2.4", "an indexed field", 4096, 3, 4),
    ("C.3", "requests without Huffman coding", 4096, 4, 7),
    ("C.4", "requests with Huffman coding", 4096, 7, 10),
    ("C.5", "responses without Huffman coding, evicting", 256, 10, 13),
    ("C.6", "responses with Huffman coding, evicting", 256, 13, 16),
]


def split_field(s):
    if ": " not in s:
        return s.rstrip(":"), ""
    i = s.index(": ", 1)
    return s[:i], s[i + 2 :]


def escape(s):
    return s.replace("\\", "\\\\").replace('"', '\\"')


def parse(text):
    c = text.index("Appendix C.  Examples")
    lines = [
        line
        for line in text[c:].splitlines()
        if not line.startswith("Peon & Ruellan") and not line.startswith("RFC 7541") and "\f" not in line
    ]

    sections, current = [], None
    for line in lines:
        m = re.match(r"^(C\.[2-6]\.\d)\.\s+(.*)$", line)
        if m:
            current = {"key": m.group(1), "title": m.group(2), "lines": []}
            sections.append(current)
            continue
        if re.match(r"^(C\.\d)\.\s", line) or line.startswith("Authors"):
            current = None
        if current:
            current["lines"].append(line)

    examples = []
    for section in sections:
        ls = section["lines"]

        i = next(k for k, line in enumerate(ls) if "Hex dump of encoded data:" in line)
        encoded = ""
        for line in ls[i + 1 :]:
            if "Decoding process:" in line:
                break
            m = re.match(r"^\s+([0-9a-f]{2,4}(?: [0-9a-f]{2,4})*)\s+\|", line)
            if m:
                encoded += m.group(1).replace(" ", "")

        j = next(k for k, line in enumerate(ls) if "dynamic table (after decoding):" in line.lower())
        table, size = [], None
        if ls[j].strip().lower().endswith("empty."):
            size = 0
        for line in [] if size == 0 else ls[j + 1 :]:
            if "Table size:" in line:
                size = int(line.split(":")[1])
                break
            m = re.match(r"^\s+\[\s*\d+\]\s+\(s =\s*(\d+)\)\s+(.*)$", line)
            if m:
                table.append([int(m.group(1)), m.group(2)])
            elif line.strip() and table:
                table[-1][1] += " " + line.strip()  # an entry wrapped onto the next line

        entries = []
        for entry_size, text_ in table:
            name, value = split_field(text_)
            assert entry_size == len(name) + len(value) + 32, (section["key"], entry_size, name, value)
            entries.append((name, value))
        assert size == sum(s for s, _ in table), (section["key"], size)

        k = next(k for k, line in enumerate(ls) if "Decoded header list:" in line)
        fields = []
        for line in ls[k + 1 :]:
            if not line.strip():
                continue
            if not line.startswith("   ") or line.startswith("    "):
                break
            fields.append(split_field(line.strip()))

        examples.append((section, encoded, fields, entries, size))

    assert len(examples) == 16, [s["key"] for s, *_ in examples]
    return examples


def main(path):
    examples = parse(open(path, encoding="utf-8").read())

    def pairs(fields):
        return "{" + ", ".join(f'{{"{escape(n)}", "{escape(v)}"}}' for n, v in fields) + "}"

    out = [
        "// RFC 7541, Appendix C (generated from its text by tools/hpack/gen_examples.py): each sequence is decoded in",
        "// order by one decoder.",
        "std::vector<sequence> rfc_examples()",
        "{",
        "    return {",
    ]
    for key, title, table_size, first, last in GROUPS:
        out += [
            "        sequence{",
            f'            .name       = "{key}: {title}",',
            f"            .table_size = {table_size},",
            "            .examples   = {",
        ]
        for section, encoded, fields, entries, size in examples[first:last]:
            out += [
                f"                // {section['key']}. {section['title']}",
                "                example{",
                f'                    .encoded    = "{encoded}",',
                f"                    .fields     = {pairs(fields)},",
                f"                    .table      = {pairs(entries)},",
                f"                    .table_size = {size},",
                "                },",
            ]
        out += ["            },", "        },"]
    out += ["    };", "}"]
    print("\n".join(out))


if __name__ == "__main__":
    main(sys.argv[1])
