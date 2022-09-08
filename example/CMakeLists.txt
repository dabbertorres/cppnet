cmake_minimum_required(VERSION 3.18)

# use vcpkg for dependency management
include("../cmake/vcpkg.cmake")

# for use with clangd and other libclang tools
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(cxxopts CONFIG REQUIRED)
find_package(fmt CONFIG REQUIRED)

add_executable(net-example main.cpp)

target_include_directories(
  net-example
  PRIVATE
  "${CMAKE_SOURCE_DIR}/include/"
  cxxopts::cxxopts
  fmt::fmt
)

target_link_libraries(
  net-example
  PRIVATE
  cxxopts::cxxopts
  fmt::fmt
)

target_compile_features(
  net-example
  PRIVATE cxx_std_20
)

if(MSVC)
  target_compile_options(
    net-example
    PRIVATE
    /W4
  )
else()
  target_compile_options(
    net-example
    PRIVATE
    -fstack-usage
    -pedantic-errors
    -Wall
    -Wconversion
    -Wdocumentation
    -Wdouble-promotion
    -Wextra
    -Wformat-pedantic
    -Wloop-analysis
    -Wold-style-cast
    -Wpacked
    # -Wpadded
    -Wpedantic
    -Wshadow
    -Wsuggest-override
    -Wthread-safety
    -Wundef
    -Wundefined-func-template
    -Wundefined-reinterpret-cast
    -Wunreachable-code-aggressive
  )
endif()
