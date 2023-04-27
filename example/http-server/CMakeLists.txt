cmake_minimum_required(VERSION 3.18)

# use vcpkg for dependency management
include("../../cmake/vcpkg.cmake")

# for use with clangd and other libclang tools
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_executable(net-example-http-server main.cpp)

target_include_directories(
  net-example-http-server
  PRIVATE
  "${CMAKE_SOURCE_DIR}/include/"
)

target_link_libraries(
  net-example-http-server
  PRIVATE
  net
)

target_compile_features(
  net-example-http-server
  PRIVATE cxx_std_20
)

if(MSVC)
  target_compile_options(
    net-example-http-server
    PRIVATE
    /W4
  )
else()
  target_compile_options(
    net-example-http-server
    PRIVATE
    -Wall
    -Wextra
    -pedantic
  )
endif()
