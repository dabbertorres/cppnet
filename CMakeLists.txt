cmake_minimum_required(VERSION 3.18)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "Do not build in-source. Please remove CMakeCache.txt and the CMakeFiles/ directory. Then build out-of-source.")
endif()

# use vcpkg for dependency management
#if(NOT CMAKE_TOOLCHAIN_FILE)
#    if(DEFINED ENV{VCPKG_ROOT})
#        set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
#            CACHE STRING "Vcpkg toolchain file")
#    else()
#        set(CMAKE_TOOLCHAIN_FILE ${CMAKE_CURRENT_SOURCE_DIR}/vcpkg/scripts/buildsystems/vcpkg.cmake
#            CACHE STRING "Vcpkg toolchain file")
#    endif()
#endif()

# for use with clangd and other libclang tools
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

project(
    "net"
    VERSION 0.1.0
    LANGUAGES C CXX
)

include(CTest)

file(GLOB_RECURSE sources CONFIGURE_DEPENDS "src/*.cpp")
add_library(net ${sources})

target_include_directories(net
    PUBLIC
        "include/"
)

target_compile_features(net
    PRIVATE cxx_std_20
)

if(MSVC)
    target_compile_options(net
        PRIVATE
            /W4
    )
else()
    target_compile_options(net
        PRIVATE
            -fstack-usage
            -pedantic-errors
            -Wall
            -Wconversion
            -Wdocumentation
            -Wdouble-promotion
            -Wextra
            -Wformat-pedantic
            -Wloop-analysis
            -Wold-style-cast
            -Wpacked
            -Wpadded
            -Wpedantic
            -Wshadow
            -Wsuggest-override
            -Wthread-safety
            -Wundef
            -Wundefined-func-template
            -Wundefined-reinterpret-cast
            -Wunreachable-code-aggressive
    )
endif()

if(BUILD_TESTING)
    add_subdirectory("test/")
endif()
