cmake_minimum_required(VERSION 3.18)

# use vcpkg for dependency management
include("../../cmake/vcpkg.cmake")

# for use with clangd and other libclang tools
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_executable(net-example-ip-resolv main.cpp)

#target_include_directories(
#  net-example-ip-resolv
#  PRIVATE "${CMAKE_SOURCE_DIR}/include/"
#)

target_link_libraries(
  net-example-ip-resolv
  PRIVATE net
)

#target_compile_features(
#  net-example-ip-resolv
#  PRIVATE cxx_std_23
#)
