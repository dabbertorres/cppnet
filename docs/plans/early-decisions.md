# Decisions from the first reviews

Status: done. The first in-depth review, its lint passes and the developer-experience review (2026-10-14 to 2026-10-17)
came before there was a plan per area, so the user's choices then are recorded here. Later decisions are in each
area's plan.

## Errors

- **Errors are `std::error_code`s, compared with conditions** (2026-10-17, replacing `std::expected<T,
  std::error_condition>` from 2026-10-15). A review found `err == status_condition::timed_out` always false, as
  comparing two conditions is exact, and reporting conditions loses the original error and makes the library's own
  equivalences dead code. So failures report codes (errno in `std::system_category()`, or the library's enums in their
  categories), and callers compare them with conditions. `std::optional` stays for values that are legitimately absent.
- **Running out of memory is fatal**: `std::bad_alloc` (and `bad_array_new_length`, `length_error`) isn't caught, and
  clang-tidy's `bugprone-exception-escape` ignores them, rather than every `noexcept` path guarding against them.

## Style and lint

- **Aggregates' members have default initializers (`{}`)**, so designated initializers name only what they set. First
  the user kept `-Wmissing-designated-field-initializers` and spelled every field out; then chose the defaults, and
  `readability-redundant-member-init` is off.
- **Implicit conversions on purpose stay**: `explicit` was added to the constructors clang-tidy found, except
  `ip_addr`'s and `result`'s, which are meant as conversions (`NOLINT`ed).
- **Include-what-you-use rather than `misc-include-cleaner`**, which disagreed with it in 161 places and is off.
- **`std::scoped_lock`** for exclusive locks (the user replaced every `std::lock_guard`, 2026-10-15), and **`util::defer`**
  for code run at scope exit.
- **`util::string_map`** exists for heterogeneous lookup: extend it rather than work around it.

## Runtime and API

- **`coro::task`'s await handshake has one code path** (the user's choice over a second, specialized one): an atomic
  exchange per `co_await` decides which side continues, so loops of awaits that finish without suspending don't grow
  the stack. `bench_task` measures it.
- **Request-scoped values are explicit**: middleware passes them to handlers in `server_request::extensions` (a
  `util::type_map`), and `http_tracing` puts its `instrument::trace` there, rather than an ambient, task-local
  context.
- **glaze, not nlohmann-json**, for JSON (OTLP), entirely: in `bench_web`'s JSON test it took 129 ns and one allocation
  against 937 ns and 11.
- **TLS doesn't require Extended Master Secret by default**, as browsers, Go and OpenSSL don't
  (`require_extended_master_secret` opts in); Botan's error text is kept, and alerts say which side sent them.

## Tests

- **Retries in tests are the exception.** When a test client's connect raced the server starting to listen, the fix
  was the library's (the server listens in its constructor), which helps real users too; a retry is for what nothing
  better can order (an external process, the OS).
