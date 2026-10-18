# net for C++

A C++23 networking library, built on its own coroutine runtime: TCP sockets, DNS lookups, URLs, an HTTP/1.1 client,
an HTTP/1.1 and HTTP/2 server (with a router, and middleware for Prometheus metrics and OpenTelemetry tracing), and
TLS (with Botan), for HTTPS in both.

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
ctest --test-dir build -C Debug       # the tests (some need the network)
```

The first configure builds the dependencies, which takes a while (Botan, for TLS, is most of it: several minutes);
vcpkg caches them afterwards. To build without TLS (and Botan), add `-DNET_TLS=OFF`.

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
fuzz targets' short runs.

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
it (see cppnet's `vcpkg.json`, and its overlay port for Botan in `ports/`).

## License

MIT: see [LICENSE.md](LICENSE.md).
