cmake_minimum_required(VERSION 3.18)

# use vcpkg for dependency management
include("../cmake/vcpkg.cmake")

# for use with clangd and other libclang tools
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Catch2 CONFIG REQUIRED)

file(GLOB_RECURSE test_sources CONFIGURE_DEPENDS "*.cpp")
if(NOT NET_TLS)
  list(FILTER test_sources EXCLUDE REGEX "/test/tls/")
endif()
add_executable(tests ${test_sources})

target_include_directories(tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" Catch2::Catch2)

target_link_libraries(tests PRIVATE Catch2::Catch2 Catch2::Catch2WithMain net)

# the TLS tests make their certificates with Botan.
if(NET_TLS)
  target_link_libraries(tests PRIVATE ${NET_BOTAN_TARGET})
endif()

include(Catch)
catch_discover_tests(tests)
