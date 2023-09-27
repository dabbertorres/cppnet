cmake_minimum_required(VERSION 3.18)

# use vcpkg for dependency management
include("../cmake/vcpkg.cmake")

# for use with clangd and other libclang tools
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Catch2 CONFIG REQUIRED)

file(GLOB_RECURSE test_sources CONFIGURE_DEPENDS "*.cpp")
add_executable(tests ${test_sources})

target_include_directories(
  tests
  PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  Catch2::Catch2
)

target_link_libraries(
  tests
  PRIVATE
  Catch2::Catch2
  Catch2::Catch2WithMain
  net
)

include(Catch)
catch_discover_tests(tests)
