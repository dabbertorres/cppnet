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
  spdlog::spdlog
)

target_compile_features(
  net
  PRIVATE cxx_std_23
)

if(MSVC)
  target_compile_options(
    net
    PRIVATE
    /W4
  )

  # target_link_options(
  #   net
  #   PRIVATE
  # )
else()
  # find_program(MOLD "mold")
  # if(MOLD)
  #   target_link_options(
  #     net
  #     PUBLIC
  #     -fuse-ld=mold
  #   )
  # endif()

  if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
    target_compile_options(
      net
      PRIVATE
      -fdiagnostics-color=always
    )
  elseif("${CMAKE_CXX_COMPILER_ID}" STREQUAL "Clang")
    target_compile_options(
      net
      PRIVATE
      -fcolor-diagnostics
    )
  endif()

  set(SANITIZERS address,undefined)

  target_compile_options(
    net
    PRIVATE
    -Wall
    -Wextra
    -pedantic
    PUBLIC
    $<$<CONFIG:Debug>:-fsanitize=${SANITIZERS}>
  )

  target_link_options(
    net
    PUBLIC
    $<$<CONFIG:Debug>:-fsanitize=${SANITIZERS}>
  )
endif()

if(BUILD_TESTING)
  include(CTest)
  add_subdirectory("test/")
endif()

if(BUILD_EXAMPLE)
  add_subdirectory("example/")
endif()
