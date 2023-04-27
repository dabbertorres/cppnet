cmake_minimum_required(VERSION 3.18)

# use vcpkg for dependency management
include("../../cmake/vcpkg.cmake")

# for use with clangd and other libclang tools
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_executable(net-example-ip-resolv main.cpp)

target_include_directories(
  net-example-ip-resolv
  PRIVATE
  "${CMAKE_SOURCE_DIR}/include/"
)

target_link_libraries(
  net-example-ip-resolv
  PRIVATE
  net
)

target_compile_features(
  net-example-ip-resolv
  PRIVATE cxx_std_20
)

if(MSVC)
  target_compile_options(
    net-example-ip-resolv
    PRIVATE
    /W4
  )
else()
  target_compile_options(
    net-example-ip-resolv
    PRIVATE
    -Wall
    -Wextra
    -pedantic
  )
endif()
