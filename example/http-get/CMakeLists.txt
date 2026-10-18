cmake_minimum_required(VERSION 3.18)

# for use with clangd and other libclang tools
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_executable(net-example-http-get main.cpp)

target_link_libraries(net-example-http-get PRIVATE net)
