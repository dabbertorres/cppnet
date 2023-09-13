# hack in using libc++ when building dependencies from vcpkg...
if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "Clang")
  set(
    VCPKG_C_FLAGS
    "-stdlib=libc++"
  )
  set(
    VCPKG_CXX_FLAGS
    "-stdlib=libc++"
  )
endif()

# use vcpkg for dependency management

if(NOT CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VCPKG_ROOT})
    set(
      CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
      CACHE STRING "Vcpkg toolchain file"
    )
  else()
    set(
      CMAKE_TOOLCHAIN_FILE ${CMAKE_CURRENT_SOURCE_DIR}/vcpkg/scripts/buildsystems/vcpkg.cmake
      CACHE STRING "Vcpkg toolchain file"
    )
  endif()
endif()
