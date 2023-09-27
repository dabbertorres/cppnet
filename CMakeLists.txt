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

target_link_libraries(
  net
  PUBLIC spdlog::spdlog
  PRIVATE nlohmann_json::nlohmann_json
)

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
