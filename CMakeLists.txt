cmake_minimum_required(VERSION 3.18)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
  message(FATAL_ERROR "Do not build in-source. Please remove CMakeCache.txt and the CMakeFiles/ directory. Then build out-of-source.")
endif()

# for use with clangd and other libclang tools
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_CXX_EXTENSIONS OFF)

# use vcpkg for dependency management
include("cmake/vcpkg.cmake")

project(
  "net"
  VERSION 0.1.0
  LANGUAGES C CXX
)

file(GLOB_RECURSE sources CONFIGURE_DEPENDS "src/*.cpp")
add_library(net ${sources})

target_include_directories(
  net
  PUBLIC
  "${CMAKE_SOURCE_DIR}/include/"
)

target_link_libraries(
  net
  PRIVATE
)

target_compile_features(
  net
  PRIVATE cxx_std_20
)

if(MSVC)
  target_compile_options(
    net
    PRIVATE
    /W4
  )
else()
  target_compile_options(
    net
    PRIVATE
    -Wall
    -Wextra
    -pedantic
    # -fstack-usage
    # -pedantic-errors
    # -Wconversion
    # -Wdocumentation
    # -Wdouble-promotion
    # -Wformat-pedantic
    # -Wloop-analysis
    # -Wold-style-cast
    # -Wpacked
    # -Wpadded
    # -Wpedantic
    # -Wshadow
    # -Wsuggest-override
    # -Wthread-safety
    # -Wundef
    # -Wundefined-func-template
    # -Wundefined-reinterpret-cast
    # -Wunreachable-code-aggressive
  )
endif()

if(BUILD_TESTING)
  include(CTest)
  add_subdirectory("test/")
endif()

if(BUILD_EXAMPLE)
  add_subdirectory("example/")
endif()
