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

find_package(spdlog CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

file(GLOB_RECURSE sources CONFIGURE_DEPENDS "src/*.cpp")
add_library(net ${sources})

target_include_directories(
  net
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include/"
)

target_compile_features(
  net
  PUBLIC cxx_std_23
)

target_compile_definitions(
  net
  PRIVATE
  SPDLOG_COMPILED_LIB=1
)

target_link_libraries(
  net
  PUBLIC spdlog::spdlog
  PRIVATE nlohmann_json::nlohmann_json
)

if(BUILD_TESTING)
  include(CTest)
  add_subdirectory("test/")
endif()

# Benchmarks for implementation choices (see bench/CMakeLists.txt). Built with the tests, by default.
option(BUILD_BENCHMARKS "Build the benchmarks" ${BUILD_TESTING})
if(BUILD_BENCHMARKS)
  add_subdirectory("bench/")
endif()

if(BUILD_EXAMPLE)
  file(GLOB example_dirs LIST_DIRECTORIES true "example/*")
  foreach(dir ${example_dirs})
    if(IS_DIRECTORY ${dir})
      add_subdirectory(${dir})
    else()
      continue()
    endif()
  endforeach()
endif()

# libFuzzer targets for the parsers (see fuzz/CMakeLists.txt): AUTO builds them with the tests, if the compiler supports
# libFuzzer (LLVM's clang does; Apple's clang and GCC don't). ON requires them, and OFF skips them.
set(BUILD_FUZZING "AUTO" CACHE STRING "Build the fuzz targets: AUTO, ON, or OFF")
set_property(CACHE BUILD_FUZZING PROPERTY STRINGS AUTO ON OFF)

if(BUILD_FUZZING STREQUAL "ON" OR (BUILD_FUZZING STREQUAL "AUTO" AND BUILD_TESTING))
  include(CheckCXXSourceCompiles)

  set(CMAKE_REQUIRED_FLAGS "-fsanitize=fuzzer")
  set(CMAKE_REQUIRED_LINK_OPTIONS "-fsanitize=fuzzer")
  check_cxx_source_compiles(
    "#include <cstddef>
     #include <cstdint>
     extern \"C\" int LLVMFuzzerTestOneInput(const std::uint8_t*, std::size_t) { return 0; }"
    NET_HAS_LIBFUZZER
  )
  unset(CMAKE_REQUIRED_FLAGS)
  unset(CMAKE_REQUIRED_LINK_OPTIONS)

  if(NET_HAS_LIBFUZZER)
    add_subdirectory("fuzz/")
  elseif(BUILD_FUZZING STREQUAL "ON")
    message(FATAL_ERROR "BUILD_FUZZING is ON, but ${CMAKE_CXX_COMPILER_ID} can't build libFuzzer targets")
  else()
    message(STATUS "Skipping the fuzz targets: ${CMAKE_CXX_COMPILER_ID} can't build libFuzzer targets")
  endif()
endif()
