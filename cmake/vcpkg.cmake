# use vcpkg for dependency management
#
# Include this before project(): by cppnet's CMakeLists.txt, or by a project that builds cppnet with
# add_subdirectory() (see README.md), so that vcpkg installs cppnet's dependencies (with cppnet's
# manifest, unless the project has its own).

# TLS (e.g. HTTPS), with Botan: vcpkg's "tls" feature, which is on by default. Turning it off skips
# building Botan.
option(NET_TLS "Build TLS support (with Botan)" ON)
if(NOT NET_TLS)
  set(VCPKG_MANIFEST_NO_DEFAULT_FEATURES ON)
endif()

# Prefer the submodule, which matches the manifest's builtin-baseline. $VCPKG_ROOT is only a
# fallback, for when the submodule hasn't been checked out: another vcpkg may not have the baseline
# commit.
if(NOT CMAKE_TOOLCHAIN_FILE)
  if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../vcpkg/scripts/buildsystems/vcpkg.cmake")
    set(CMAKE_TOOLCHAIN_FILE
        "${CMAKE_CURRENT_LIST_DIR}/../vcpkg/scripts/buildsystems/vcpkg.cmake"
        CACHE STRING "Vcpkg toolchain file"
    )
  elseif(DEFINED ENV{VCPKG_ROOT})
    set(CMAKE_TOOLCHAIN_FILE
        "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
        CACHE STRING "Vcpkg toolchain file"
    )
  else()
    message(
      FATAL_ERROR "vcpkg not found: run `git submodule update --init` in cppnet, or set VCPKG_ROOT."
    )
  endif()
endif()

# vcpkg reads the top-level project's manifest: if that's another project without one of its own,
# cppnet's.
if(NOT DEFINED VCPKG_MANIFEST_DIR AND NOT EXISTS "${CMAKE_SOURCE_DIR}/vcpkg.json")
  set(VCPKG_MANIFEST_DIR
      "${CMAKE_CURRENT_LIST_DIR}/.."
      CACHE PATH "The vcpkg manifest's directory"
  )
endif()
