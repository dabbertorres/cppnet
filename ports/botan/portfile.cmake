vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO randombit/botan
    REF "${VERSION}"
    SHA512 33a47e5733aedc29704652bf16704a2cd080cdcebd01c95879b5db3af3186debbef711c53ef131a9cb7defe3dd8ffae49d6efe7e75aef9107473b92dc6b3f101
    HEAD_REF master
    PATCHES
        verbose-install.patch
        configure-zlib.patch
        fix_android.patch
        # cppnet: files that use the standard library without including what they use (e.g. std::fill without
        # <algorithm>), which libc++ 23 no longer includes along with other headers.
        libcxx-missing-includes.patch
)
file(COPY "${CMAKE_CURRENT_LIST_DIR}/configure" DESTINATION "${SOURCE_PATH}")

if(VCPKG_TARGET_IS_MINGW)
    vcpkg_check_linkage(ONLY_STATIC_LIBRARY)
endif()

vcpkg_find_acquire_program(PYTHON3)

vcpkg_cmake_get_vars(cmake_vars_file)
include("${cmake_vars_file}")

set(pkgconfig_syntax "")
if(VCPKG_DETECTED_CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    set(pkgconfig_syntax "--msvc-syntax")
endif()

vcpkg_list(SET configure_arguments
    "--distribution-info=vcpkg ${TARGET_TRIPLET}"
    --disable-cc-tests
    --with-pkg-config
    --with-cmake-config
    --link-method=copy
    --with-debug-info
    --without-include-namespace
    --includedir=include
    --bindir=bin
    --libdir=lib
    --cmakeconfigdir=share/${PORT}
    --without-documentation
    "--with-external-includedir=${CURRENT_INSTALLED_DIR}/include"
)
vcpkg_list(SET pkgconfig_requires)

# cppnet: only the modules cppnet uses (with their dependencies, which configure.py adds), rather than all of Botan:
# about half its sources. See docs/plans/tls.md ("Trimming Botan"). cppnet's TLS policy offers only these (whichever
# Botan it's built against), so a missing one fails its tests (test/tls/stream.cpp's handshakes per group, cipher suite
# and kind of certificate) rather than a handshake in the field. Botan looks some algorithms up by name when they run
# (e.g. a signature's padding or hash), which configure.py can't see, so those are named here too.
vcpkg_list(SET cppnet_modules
    # TLS 1.2 and 1.3, and 1.3's hybrid post-quantum key exchange (X25519MLKEM768, and Botan's P-256 and P-384 hybrids).
    tls12
    tls13
    tls13_pqc
    # The key exchange groups: Go's (X25519, P-256, P-384, P-521, and ML-KEM for the hybrids). Botan's TLS needs its
    # finite field Diffie-Hellman (dh) too, which it adds, but cppnet's policy doesn't offer it.
    x25519
    ecdh
    ml_kem
    pcurves_secp256r1
    pcurves_secp384r1
    pcurves_secp521r1
    # Certificates: X.509 (path validation; and X509_CA and PKCS #10, which the tests make theirs with), PEM, and ECDSA
    # and RSA keys and signatures (ECDSA's nonces as RFC 6979 hedges them; RSA's PKCS #1 v1.5 and PSS signatures, and
    # PKCS #1 v1.5 encryption, which TLS 1.2 needs).
    x509
    pem
    ecdsa
    rfc6979
    rsa
    emsa_pkcs1
    emsa_pssr
    eme_pkcs1
    # The system's trust store: the keychain on Apple's platforms (skipped elsewhere), or a bundle of certificates (e.g.
    # Linux's, which configure.py finds).
    certstor_system
    certstor_system_macos
    certstor_flatfile
    # The ciphers (TLS's AEADs: AES-GCM and ChaCha20-Poly1305), hashes and key derivation.
    aes
    gcm
    chacha20poly1305
    sha2_32
    sha2_64
    hmac
    hkdf
    prf_tls
    # The RNG: the operating system's.
    system_rng
    # The CPUs' versions of the above (AES-NI, CLMUL, SHA extensions, AVX2, AVX-512, ARMv8's, ...): configure.py skips
    # those for other CPUs.
    aes_*
    ghash_*
    ctr_*
    chacha_*
    poly1305_*
    sha1_*
    sha2_32_*
    sha2_64_*
    keccak_perm_*
)
list(JOIN cppnet_modules "," cppnet_modules)
vcpkg_list(APPEND configure_arguments --minimized-build "--enable-modules=${cppnet_modules}")

# cppnet: chacha_* also names ChaCha's RNG, which isn't a CPU's version (and isn't used).
vcpkg_list(SET cppnet_disabled_modules chacha_rng)

if("amalgamation" IN_LIST FEATURES)
    vcpkg_list(APPEND configure_arguments --amalgamation)
endif()

set(ZLIB_LIBS_RELEASE "")
set(ZLIB_LIBS_DEBUG "")
if("zlib" IN_LIST FEATURES)
    vcpkg_list(APPEND configure_arguments --with-zlib)
    vcpkg_list(APPEND pkgconfig_requires zlib)
    x_vcpkg_pkgconfig_get_modules(LIBS PREFIX "ZLIB" MODULES "zlib" ${pkgconfig_syntax})
endif()

if(VCPKG_TARGET_IS_EMSCRIPTEN)
    vcpkg_list(APPEND configure_arguments --cpu=wasm)
elseif(VCPKG_TARGET_ARCHITECTURE STREQUAL "x86")
    vcpkg_list(APPEND configure_arguments --cpu=x86)
elseif(VCPKG_TARGET_ARCHITECTURE STREQUAL "x64")
    vcpkg_list(APPEND configure_arguments --cpu=x86_64)
elseif(VCPKG_TARGET_ARCHITECTURE STREQUAL "arm")
    vcpkg_list(APPEND configure_arguments --cpu=arm32)
elseif(VCPKG_TARGET_ARCHITECTURE STREQUAL "arm64")
    vcpkg_list(APPEND configure_arguments --cpu=arm64)
else()
    message(FATAL_ERROR "Unsupported architecture")
endif()

# Allow disabling use of WinSock2 by setting BOTAN_USE_WINSOCK2=OFF in triplet
# for targeting older Windows versions with missing APIs.
if(VCPKG_TARGET_IS_WINDOWS AND DEFINED BOTAN_USE_WINSOCK2 AND NOT BOTAN_USE_WINSOCK2)
    vcpkg_list(APPEND configure_arguments --without-os-features=winsock2)
endif()

if(VCPKG_TARGET_IS_WINDOWS AND NOT VCPKG_TARGET_IS_MINGW)
    vcpkg_list(APPEND configure_arguments --os=windows)

    if(VCPKG_DETECTED_CMAKE_CXX_COMPILER MATCHES "clang-cl(\.exe)?$")
        vcpkg_list(APPEND configure_arguments "--cc=clangcl")
    elseif(VCPKG_DETECTED_CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        vcpkg_list(APPEND configure_arguments "--cc=msvc")
    endif()

    vcpkg_list(APPEND configure_arguments "--extra-cxxflags=${VCPKG_DETECTED_CMAKE_CXX_FLAGS}")

    # cppnet: only the library, not Botan's command-line tool.
    list(JOIN cppnet_disabled_modules "," cppnet_disabled_modules)
    vcpkg_list(APPEND configure_arguments "--disable-modules=${cppnet_disabled_modules}")
    if(VCPKG_LIBRARY_LINKAGE STREQUAL "dynamic")
        vcpkg_list(APPEND configure_arguments --build-targets=shared)
    else()
        vcpkg_list(APPEND configure_arguments --build-targets=static)
    endif()

    if(VCPKG_CRT_LINKAGE STREQUAL "dynamic")
        set(BOTAN_MSVC_RUNTIME MD)
    else()
        set(BOTAN_MSVC_RUNTIME MT)
    endif()

    vcpkg_install_nmake(
        SOURCE_PATH "${SOURCE_PATH}"
        PROJECT_NAME "Makefile"
        PREFER_JOM
        PRERUN_SHELL_RELEASE
            "${PYTHON3}" "${SOURCE_PATH}/configure.py"
            ${configure_arguments}
            "--prefix=${CURRENT_PACKAGES_DIR}"
            "--msvc-runtime=${BOTAN_MSVC_RUNTIME}"
            "--with-external-libdir=${CURRENT_INSTALLED_DIR}/lib"
        PRERUN_SHELL_DEBUG
            "${PYTHON3}" "${SOURCE_PATH}/configure.py"
            ${configure_arguments}
            "--prefix=${CURRENT_PACKAGES_DIR}/debug"
            "--msvc-runtime=${BOTAN_MSVC_RUNTIME}d"
            "--with-external-libdir=${CURRENT_INSTALLED_DIR}/debug/lib"
            --debug-mode
        OPTIONS
            "CXX=\"${VCPKG_DETECTED_CMAKE_CXX_COMPILER}\""
            "LINKER=\"${VCPKG_DETECTED_CMAKE_LINKER}\""
            "AR=\"${VCPKG_DETECTED_CMAKE_AR}\""
            "EXE_LINK_CMD=\"${VCPKG_DETECTED_CMAKE_LINKER}\" ${VCPKG_LINKER_FLAGS}"
        OPTIONS_RELEASE
            "ZLIB_LIBS=${ZLIB_LIBS_RELEASE}"
        OPTIONS_DEBUG
            "ZLIB_LIBS=${ZLIB_LIBS_DEBUG}"
    )
    vcpkg_copy_pdbs()
else()
    if(VCPKG_TARGET_IS_ANDROID)
        vcpkg_list(APPEND configure_arguments --os=android)
    elseif(VCPKG_TARGET_IS_EMSCRIPTEN)
        vcpkg_list(APPEND configure_arguments --os=emscripten)
    elseif(VCPKG_TARGET_IS_MINGW)
        vcpkg_list(APPEND configure_arguments --os=mingw)
    endif()

    if(VCPKG_TARGET_IS_EMSCRIPTEN)
        vcpkg_list(APPEND configure_arguments --cc=emcc)
    elseif(VCPKG_DETECTED_CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        vcpkg_list(APPEND configure_arguments --cc=gcc)

        # cppnet: GCC on macOS can't compile what's left out here:
        # - the keychain's trust store, as Apple's headers use blocks, which only Clang supports. Botan's system trust
        #   store reads /etc/ssl/cert.pem (Apple's bundle of the system's roots) instead, which configure.py finds.
        # - on Apple Silicon, the ARMv8 SM3 code (which TLS doesn't use), as GCC writes SM3's instructions (e.g.
        #   "sm3tt1a v30.4s, v29.4s, v31.4s[0]") in a form Apple's assembler rejects.
        #   (No longer loaded anyway, as the build is minimized; still disabled, should it ever be.)
        if(VCPKG_TARGET_IS_OSX)
            vcpkg_list(APPEND cppnet_disabled_modules certstor_system_macos)
            if(VCPKG_TARGET_ARCHITECTURE STREQUAL "arm64")
                vcpkg_list(APPEND cppnet_disabled_modules sm3_armv8)
            endif()
        endif()
    elseif(VCPKG_DETECTED_CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        vcpkg_list(APPEND configure_arguments --cc=clang)
    endif()

    list(JOIN cppnet_disabled_modules "," cppnet_disabled_modules)
    vcpkg_list(APPEND configure_arguments "--disable-modules=${cppnet_disabled_modules}")

    # cppnet: only the library, not Botan's command-line tool (which cppnet doesn't use).
    if(VCPKG_LIBRARY_LINKAGE STREQUAL "dynamic")
        vcpkg_list(APPEND configure_arguments --build-targets=shared)
    else()
        vcpkg_list(APPEND configure_arguments --build-targets=static)
    endif()

    # botan's install.py doesn't handle DESTDIR on windows host,
    # so we must avoid the standard '--prefix' and 'DESTDIR' install.
    vcpkg_configure_make(
        SOURCE_PATH "${SOURCE_PATH}"
        DISABLE_VERBOSE_FLAGS
        NO_ADDITIONAL_PATHS
        OPTIONS
            "${PYTHON3}" "${SOURCE_PATH}/configure.py"
            ${configure_arguments}
        OPTIONS_RELEASE
            "--prefix=${CURRENT_PACKAGES_DIR}"
            "--with-external-libdir=${CURRENT_INSTALLED_DIR}/lib"
        OPTIONS_DEBUG
            --debug-mode
            "--prefix=${CURRENT_PACKAGES_DIR}/debug"
            "--with-external-libdir=${CURRENT_INSTALLED_DIR}/debug/lib"
    )
    vcpkg_build_make(
        BUILD_TARGET install
        OPTIONS
            "ZLIB_LIBS_RELEASE=${ZLIB_LIBS_RELEASE}"
            "ZLIB_LIBS_DEBUG=${ZLIB_LIBS_DEBUG}"
    )
endif()

vcpkg_cmake_config_fixup()

if(pkgconfig_requires)
    list(JOIN pkgconfig_requires ", " pkgconfig_requires)
    file(APPEND "${CURRENT_PACKAGES_DIR}/lib/pkgconfig/botan-3.pc" "Requires.private: ${pkgconfig_requires}")
    if(NOT VCPKG_BUILD_TYPE)
        file(APPEND "${CURRENT_PACKAGES_DIR}/debug/lib/pkgconfig/botan-3.pc" "Requires.private: ${pkgconfig_requires}")
    endif()
endif()
vcpkg_fixup_pkgconfig()

file(REMOVE_RECURSE
    "${CURRENT_PACKAGES_DIR}/debug/include"
    "${CURRENT_PACKAGES_DIR}/debug/share"
)

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/license.txt")
