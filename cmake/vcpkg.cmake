# use vcpkg for dependency management

# Prefer the submodule, which matches the manifest's builtin-baseline. $VCPKG_ROOT is only a fallback, for when the
# submodule hasn't been checked out: another vcpkg may not have the baseline commit.
if(NOT CMAKE_TOOLCHAIN_FILE)
  if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../vcpkg/scripts/buildsystems/vcpkg.cmake")
    set(
      CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_LIST_DIR}/../vcpkg/scripts/buildsystems/vcpkg.cmake"
      CACHE STRING "Vcpkg toolchain file"
    )
  elseif(DEFINED ENV{VCPKG_ROOT})
    set(
      CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
      CACHE STRING "Vcpkg toolchain file"
    )
  else()
    message(FATAL_ERROR "vcpkg not found: run `git submodule update --init`, or set VCPKG_ROOT.")
  endif()
endif()
