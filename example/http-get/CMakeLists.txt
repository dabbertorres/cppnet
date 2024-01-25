cmake_minimum_required(VERSION 3.18)

# use vcpkg for dependency management
include("../../cmake/vcpkg.cmake")

# for use with clangd and other libclang tools
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_executable(net-example-http-get main.cpp)

target_link_libraries(
  net-example-http-get
  PRIVATE
  net
)
