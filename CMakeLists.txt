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
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include/"
)

target_compile_features(
  net
  PUBLIC cxx_std_20
)

target_link_libraries(
  net
  PUBLIC spdlog::spdlog
)

if(MSVC)
  target_compile_options(
    net
    PRIVATE
    /W4
  )
else()
  if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
    set(
      COMPILER_SPECIFIC_FLAGS
      "-fdiagnostics-color=always"
      "-Wno-c++20-extensions"
    )
  elseif("${CMAKE_CXX_COMPILER_ID}" STREQUAL "Clang")
    set(
      COMPILER_SPECIFIC_FLAGS
      "-fcolor-diagnostics"
      # "-stdlib=libc++"
    )

    if(${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
      set(
        LINKER_OS_FLAGS
        "-L/usr/local/opt/llvm/lib/c++ -Wl,-rpath,/usr/local/opt/llvm/lib/c++"
      )
    endif()
  endif()

  set(SANITIZERS address,undefined)

  target_compile_options(
    net
    PRIVATE
    -Wall
    -Wextra
    -pedantic
    ${COMPILER_SPECIFIC_FLAGS}
    PUBLIC
    $<$<CONFIG:Debug>:-fsanitize=${SANITIZERS}>
  )

  target_link_options(
    net
    PRIVATE
    ${LINKER_OS_FLAGS}
    PUBLIC
    $<$<CONFIG:Debug>:-fsanitize=${SANITIZERS}>
  )
endif()

if(BUILD_TESTING)
  include(CTest)
  add_subdirectory("test/")
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
