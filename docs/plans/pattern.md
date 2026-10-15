# Plan: a regular expression engine, at runtime and compile time

Status: proposed (2026-10-15). The old `net::pattern` (parked since 2022, never used, and unfinished) was deleted when
this plan was written; it's in git history.

## Goal

Regular expressions without `std::regex`, which is slow, compiles patterns only at runtime, and matches by recursive
backtracking (exponential time on some patterns, and stack overflows on long inputs in some implementations), with
implementations that ABI stability keeps from being fixed.

Two requirements:

- Linear-time matching: a Thompson NFA, as RE2 and Go's `regexp` use.
- Patterns can be parsed and compiled at compile time, so that a bad pattern is a compile error and nothing is
  compiled at runtime.

## Prior art

- **RE2** compiles patterns at runtime only. Its design is the one to follow for matching: a Thompson NFA, simulated
  by a "Pike VM" (with a lazily built DFA as a fast path, which can come later, if ever).
- **CTRE** (Hana Dusíková's compile-time regular expressions) parses patterns at compile time and generates the
  matching code, but (as far as I know) that code backtracks.
- Russ Cox's articles ("Regular Expression Matching Can Be Simple And Fast", and "Regular Expression Matching: the
  Virtual Machine Approach") describe the construction and the Pike VM.

## Why C++23 makes this simpler than CTRE had it

Since C++20, `constexpr` code can use ordinary loops, `std::vector` and `std::string`, as long as whatever it allocates
is freed before the constant evaluation ends. So the parser and compiler can be ordinary code, run either at runtime or
at compile time, with only the final program's storage differing.

## Design

1. **Parser and compiler, `constexpr`.** Parse the pattern (with an error and its position on failure) and compile it,
   by Thompson's construction, into a flat program of instructions: *char*, *class* (a byte set), *any*, *split*
   (two next instructions), *jmp*, *match*, and the anchors.
2. **Two kinds of storage:**
   - `pattern::regex`: built at runtime, its program in a `std::vector`. Building returns
     `std::expected<regex, parse_error>`.
   - `pattern::static_regex<"[a-z]+">` (the pattern as a template argument, through a fixed-size string type): a
     first pass computes how many instructions the program has, then it's built into a `std::array` of that size. A
     bad pattern fails to compile.
3. **Matching, by a Pike VM, also `constexpr`.** It runs every NFA thread in lockstep over the input: O(pattern ×
   input) time, no recursion, and no catastrophic cases. Being `constexpr`, most tests can be `static_assert`s.
   Both full-match and search.

## Syntax (first version)

Bytes, not Unicode. Literals and escapes; `.`; `[...]` with ranges and negation; `|`; `(...)` for grouping; `*`, `+`
and `?`; `^` and `$`.

Later, if needed: `{m,n}`, capture groups (the Pike VM tracks them naturally), non-greedy quantifiers, and a DFA fast
path. Never: backreferences (an NFA can't do them; RE2 leaves them out too).

## Testing

- `static_assert`s for the parser, the compiler and matching.
- Table-driven Catch2 tests that run each case through both `regex` and `static_regex`, so the two agree.
- A libFuzzer target: parsing and matching never crash, and matching agrees with a tiny backtracking reference matcher
  on short patterns and inputs.

## Caveats

- Compilers limit the work of a constant expression (clang's `-fconstexpr-steps`, GCC's `-fconstexpr-ops-limit`), so
  large compile-time patterns may need those raised.
- Nothing uses it yet. Route constraints in the router (e.g. `/users/:id([0-9]+)`) would be the natural first user.

## Where it lives

`include/pattern/` and `src/pattern/` (`net::pattern`), depending on nothing but the standard library, like `util/`.
Most of it will be in headers, being `constexpr`.
