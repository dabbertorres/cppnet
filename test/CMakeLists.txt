cmake_minimum_required(VERSION 3.18)

find_package(Catch2 REQUIRED)

file(GLOB_RECURSE test_sources CONFIGURE_DEPENDS "*.cpp")
add_executable(tests ${test_sources})

target_include_directories(tests
    PRIVATE
    "${CMAKE_SOURCE_DIR}/include/"
)

target_compile_features(tests
    PRIVATE cxx_std_20
)

target_compile_options(tests
    PRIVATE -O0 -g
)

target_link_libraries(tests
    PRIVATE
        Catch2::Catch2
        net
)

include(Catch)
catch_discover_tests(tests)
