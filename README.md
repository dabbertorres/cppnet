# net for C++

A C++23 networking library, built on its own coroutine runtime: TCP and UDP sockets, DNS lookups, URLs, an HTTP/1.1
and HTTP/2 client, an HTTP/1.1 and HTTP/2 server (with a router, and middleware for Prometheus metrics and OpenTelemetry
tracing), and TLS (with Botan), for HTTPS in both, client certificates (mutual TLS) and session resumption included.

```cpp
namespace http = net::http;

net::io::scheduler scheduler{std::make_shared<net::coro::thread_pool>()};

http::router router;
router.GET("/hello/:name",
           [](const http::server_request& req, http::response_writer& resp) -> net::coro::task<void>
           {
               const auto msg = std::format("hello, {}\n", req.path_value("name"));
               auto*      body = co_await resp.send(http::status::OK, msg.size());
               co_await net::io::write_all(*body, msg);
           });

http::server server{&scheduler, std::move(router), {.port = "8080"}};
scheduler.spawn(server.serve());
```

[docs/guide.md](docs/guide.md) explains how it fits together: the runtime, errors, lifetimes, and each part of the
library. [example/](example/) has whole programs: an HTTP client (`http-get`), an HTTP server (`http-server`, which
serves HTTPS given `--cert` and `--key`), a TCP echo server (`tcp-echo-server`), and a DNS lookup (`ip-resolv`).

## Requirements

- Linux (epoll) or macOS (kqueue). Windows isn't supported.
- A C++23 compiler and standard library. CI builds with the latest GCC and Clang (GCC 16 and Clang 23, as of October
  2026) on Linux, and Homebrew's on macOS.
- CMake 3.24 or newer, and Ninja (for the presets).
- The dependencies (spdlog, glaze, Botan, and Catch2 for the tests) come from [vcpkg](https://vcpkg.io), which is a git
  submodule: nothing needs installing besides the compiler and tools.

## Building

```sh
git clone --recurse-submodules https://github.com/dabbertorres/cppnet.git   # or, in a clone: git submodule update --init
cd cppnet
cmake --preset dev-clang              # or dev-gcc
cmake --build build --config Debug
ctest --test-dir build -C Debug       # the tests
```

The first configure builds the dependencies, which takes a while (Botan, for TLS, is most of it: a few minutes, for
the parts of it cppnet uses); vcpkg caches them afterwards. To build without TLS (and Botan), add `-DNET_TLS=OFF`.

The `-clang` and `-gcc` presets use the `clang` or `gcc` first on your `PATH`: on macOS, that's Apple's Clang, unless
Homebrew's LLVM (`brew install llvm`) is first, as CI uses. The presets (in `CMakePresets.json`) write to `build/`,
with Ninja Multi-Config, so binaries land in per-configuration directories: e.g. `build/test/Debug/tests`, and
`build/example/http-get/Debug/net-example-http-get`.

| Preset                        | Builds                                                                       |
|-------------------------------|------------------------------------------------------------------------------|
| `dev-clang`, `dev-gcc`        | the library, tests, examples and benchmarks, with warnings, and in Debug, ASan and UBSan |
| `dev`, `dev-make`             | the same with CMake's default compiler, without the warnings or sanitizers (`dev-make`: with Makefiles) |
| `lib-clang`, `lib-gcc`, `lib` | only the library                                                             |

`./build/test/Debug/tests "[http]"` runs the tests tagged `[http]` (Catch2's filters), and `ctest -LE fuzz` skips the
fuzz targets' short runs. The tests that need the network (and public hosts, e.g. for DNS lookups and TLS handshakes)
are hidden: they run only when asked for, as `./build/test/Debug/tests "[network]"` does, or with ctest if configured
with `-DNET_NETWORK_TESTS=ON` (as CI is; `ctest -L network` runs only them). A filter naming another of their tags runs
them too: `"[dns]~[network]"` leaves them out.

## Using it in your project

Add cppnet to your project (e.g. as a git submodule, with its own submodules), and in your `CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.24)

# before project(): vcpkg installs cppnet's dependencies (with cppnet's manifest, unless your project has its own).
include(cppnet/cmake/vcpkg.cmake)

project(app CXX)

add_subdirectory(cppnet)

add_executable(app main.cpp)
target_link_libraries(app PRIVATE net::net)
```

cppnet's tests and examples aren't built then. If your project has its own `vcpkg.json`, add cppnet's dependencies to
it (see cppnet's `vcpkg.json`, and its overlay port for Botan in `ports/`, which builds only the parts of Botan cppnet
uses; vcpkg's own port, all of Botan, works too).

## Development

Working on cppnet itself: how it builds, tests, fuzzes, benchmarks and lints, and what CI checks.

### Building

- vcpkg is a git submodule (`vcpkg/`), pinned to a release that matches `builtin-baseline` in `vcpkg.json`.
  `cmake/vcpkg.cmake` sets the toolchain file automatically: it uses the submodule, and falls back to `$VCPKG_ROOT` only
  if the submodule isn't checked out. Don't pass `CMAKE_TOOLCHAIN_FILE` yourself. In-source builds are rejected.
- vcpkg builds the dependencies with the same compiler (it uses `CC`/`CXX`), and its default standard library, as the
  project uses: e.g. libc++ with Homebrew's Clang on macOS, libstdc++ on Linux. Clang 23 / libc++ 23 need fmt >= 12, so
  spdlog must be >= 1.16.
- The `-clang`/`-gcc` presets inherit `use-clang`/`use-gcc`, which add `-Wall -Wextra -Wpedantic`, and ASan+UBSan in
  Debug; `use-clang` also turns on libc++'s bounds checks in Debug (`_LIBCPP_HARDENING_MODE_EXTENSIVE`), so out-of-range
  indexes abort on macOS as libstdc++'s assertions do on Linux. Changing a preset's `*_INIT` flags needs `cmake --preset
  dev-clang --fresh`.
- Development happens on macOS and Linux. On macOS, `build` is a symlink to `build-clang/` (configured with the
  `dev-clang` preset, Homebrew clang), and there's no local GCC build (Homebrew's GCC lacks libasan for the presets'
  sanitizers): CI's GCC jobs are the GCC check there. On Linux, both presets build locally (`dev-clang` and `dev-gcc`,
  each with its sanitizers); point `build` at the one in use, as every preset writes to `build/`. `.clangd` reads
  `build/compile_commands.json`.
- Sources are collected with `GLOB_RECURSE ... CONFIGURE_DEPENDS`, so a new `src/**/*.cpp` or `test/**/*.cpp` file is
  picked up without editing CMake. Each subdirectory of `example/` is added automatically when `BUILD_EXAMPLE` is on
  (the `dev*` presets turn on `BUILD_TESTING` and `BUILD_EXAMPLE`).
- TLS is the CMake option `NET_TLS` (and vcpkg's manifest feature `tls`, which brings in Botan). On, `net` defines
  `NET_HAS_TLS`; off, the build skips Botan and `src/tls/`/`test/tls/`. Botan comes from a trimmed overlay port
  (`ports/botan/`): its portfile's comments and `docs/plans/tls.md` ("Trimming Botan", "A trimmed Botan (part 1)") say
  what it patches, builds and leaves out. Any edit under `ports/` changes vcpkg's ABI hash, so every cache (CI's too)
  builds Botan again, about 2 minutes per configuration. A Botan module cppnet starts using goes in the portfile's
  module list (`configure.py` can't see algorithms looked up by name at runtime); `./tests "[tls][policy]"` fails if one
  is missing. A benchmark result on TLS that changes with an unrelated change's code size is worth checking against
  Botan's loop alignment first (the portfile's comment, and the commands in `tls.md`).

### Tests

The tests use Catch2 v3 and build into a single `tests` executable. CTest cases are registered with
`catch_discover_tests`.

```sh
ctest --test-dir build -C Debug                 # all tests
./build/test/Debug/tests                        # run directly
./build/test/Debug/tests "[uuid]"               # by tag
./build/test/Debug/tests "parse single digits"  # by test name
ctest --test-dir build -C Debug -R uuid         # via ctest regex
```

- The hidden network tests (see Building, above) are tagged `[.network]`, which Catch2 reads as `[.][network]`; a spec
  naming any of their tags or names runs them (`"[dns]"` includes them).
- The resolver's tests use `test/dns/blocking_lookup.hpp` (a lookup that blocks until released) and the native back
  end's `test/dns/fake_server.hpp` (a fake nameserver on loopback, its address in an injected `dns::config`, so nothing
  reads `/etc` or needs the network): their comments give the lifetime rules.
- Stress a new or changed concurrent test once its code has settled, not after every change (commas in a test's name
  escaped as `\,`; `caffeinate -s -i` keeps a Mac awake, so leave it out on Linux). A race that shows up in about 1 run
  in 200 passes one run and fails CI later.

  ```sh
  seq 1 400 | caffeinate -s -i xargs -P 8 -I{} sh -c './build/test/Debug/tests "<spec>" >/dev/null 2>&1 || echo FAIL' | grep -c FAIL
  ```
- In ASan builds, the tests report leaks on Linux and (with LLVM's clang) macOS: `test/sanitizers.cpp` turns
  LeakSanitizer on where it isn't by default (Apple's clang doesn't support it), and suppresses known leaks in system
  libraries. `ASAN_OPTIONS`/`LSAN_OPTIONS` override it.
- `test/test_server.go` is a throwaway Go server for manual testing against the examples. It is not part of the test
  suite.

### Fuzzing

`fuzz/` has libFuzzer targets for the parsers, codecs and HTTP/2's scheduler: each file's header says which properties
it checks, besides crashes and hangs. `BUILD_FUZZING` (`AUTO`/`ON`/`OFF`, default `AUTO`) builds them with the tests
when the compiler can link libFuzzer: LLVM's clang (e.g. Homebrew's) can, Apple's clang and GCC can't. Each is also a
10-second ctest run labelled `fuzz` (`ctest -LE fuzz` skips them). For a longer run: `./build/fuzz/Debug/fuzz_url
-max_total_time=300 -max_len=256 <corpus-dir>`.

### Benchmarks

`bench/` has benchmarks for implementation choices (`BUILD_BENCHMARKS`, on with the tests): each source's header says
what it measures, its modes and arguments, and what it found; the plans' "Learned in" sections have the numbers behind
decisions. They're only meaningful in an optimized build, and nothing runs them automatically: `cmake --build build
--config Release --target bench_task && ./build/bench/Release/bench_task`. `bench/CMakeLists.txt` lists the targets;
`bench_client stream` wants a link with a round trip (e.g. netem in a Linux container), and `bench_tls writes` uses the
default `write_batch_size`.

`bench/web/run.sh [server ...]` compares the HTTP server with Go's net/http, hyper, Bun and Node on TechEmpower's
plaintext and JSON tests under `wrk` (results in `build/bench-web/`). On one laptop the JSON test is mostly bound by
each request's round trip through the kernel and the scheduler, so the pipelined plaintext test separates the servers
more.

Benchmarks are load-sensitive: measure on an otherwise idle machine (nothing else building or benchmarking),
interleaving the before and after rounds. Numbers from different machines or CPUs aren't comparable, so a baseline is
rerun on the machine measuring the change. A Mac in maintenance sleep stalls long runs unless they're under `caffeinate
-s -i`.

### Formatting and linting

- `.clang-format` handles formatting. `.cmake-format` handles CMake files (all but `ports/`, which are vcpkg's ports as
  upstream formats them; cmake-format mangles their argument lists and comments). A comment that must keep its own line
  breaks (e.g. a command example) goes between `# cmake-format: off` and `# cmake-format: on`.
- clang-format is pinned to a major version (23), from PyPI, as CI installs it: locally, use the same (`pip install
  'clang-format==23.*'` in a virtualenv, alongside `cmakelang==0.6.13` for cmake-format), not whatever Homebrew has, so
  a new major (which formats differently) is taken deliberately, with a commit that reformats. Check as CI does:

  ```sh
  git ls-files '*.cpp' '*.hpp' '*.h' | xargs <venv>/bin/clang-format --dry-run --Werror
  git ls-files 'CMakeLists.txt' '*/CMakeLists.txt' '*.cmake' ':!ports/' | xargs <venv>/bin/cmake-format --check
  ```
- `.clang-tidy` enables a broad set of checks: bugprone, cert, cppcoreguidelines, google, misc, modernize, performance,
  readability. `test/.clang-tidy` relaxes some checks that fire on Catch2 macros.
- To lint, run `clang-tidy -p build <file>` (a header too: it's linted as one, with a command inferred from the compile
  database). On macOS that's against libc++, and CI's lint is against Arch's libstdc++, which finds things libc++ hides
  (e.g. code under feature macros only libstdc++ defines, or throw paths inside its `std::variant`): only CI, or a lint
  on Linux (whose Clang builds use libstdc++ too), checks those.
- Includes follow include-what-you-use (IWYU), with the mappings in `iwyu.imp` (clang-tidy's `misc-include-cleaner` is
  off, as it disagrees). IWYU sees only the platform it runs on: it once dropped `config.hpp` from `epoll_loop.cpp`,
  which compiles epoll out on Linux without an error, so `config.hpp`'s includes carry `// IWYU pragma: keep`; after an
  IWYU pass, check the files with platform `#if`s and build on the other platform (or let CI). With Homebrew's LLVM,
  point IWYU at the same libc++ as the build, or it uses the macOS SDK's; on Linux, against libstdc++, the plain command
  is enough. `fix_includes.py --nocomments --nosafe_headers` applies its output; then clang-format only the include
  blocks.

  ```sh
  # macOS, Homebrew's LLVM
  iwyu_tool.py -p build -- -Xiwyu --mapping_file=$PWD/iwyu.imp -Xiwyu --no_fwd_decls \
      -nostdinc++ -isystem $(brew --prefix llvm)/include/c++/v1 -resource-dir $(clang++ -print-resource-dir)
  # Linux
  iwyu_tool.py -p build -- -Xiwyu --mapping_file=$PWD/iwyu.imp -Xiwyu --no_fwd_decls
  ```

### CI

`.github/workflows/cmake.yml` (whose header explains its triggers, matrix and caches) runs on pull requests to master,
weekly and by hand, not on pushes: GCC and Clang, latest major, on Arch Linux and macOS, with `NET_NETWORK_TESTS` on in
every job, plus `tsan` and `no-tls` builds of the tests, a `format` job, and clang-tidy (warnings as errors) on the
files a change touches. A job named `build` (the check `master`'s branch protection requires) passes only if every job
did. `.github/workflows/autobahn.yml` runs the Autobahn TestSuite against the WebSocket examples; it's optional and not
part of `build` (its header says when it runs). To run Autobahn's `fuzzingclient` by hand against
`example/websocket-echo` (port 9001), in a scratch directory (never the repository) with a config whose server is
`ws://host.docker.internal:9001`:

```sh
docker run --rm -v $PWD:/ab -w /ab crossbario/autobahn-testsuite wstest -m fuzzingclient -s /ab/fuzzingclient.json
```

Started in the background from a non-interactive shell, the example ignores SIGINT (inherited as ignored): stop it with
SIGTERM.

## License

MIT: see [LICENSE.md](LICENSE.md).
