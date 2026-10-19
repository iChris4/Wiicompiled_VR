cmake_minimum_required(VERSION 3.25)

# Dawn's pinned macOS artifacts target 12.0. Set the same floor before project()
# initializes the Apple toolchain so direct developer CMake invocations cannot
# accidentally inherit the running SDK's deployment version. This cache entry
# is harmless on non-Apple platforms and remains overridable by a caller.
if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING
        "Minimum macOS version supported by WiiCompiled" FORCE)
endif()
project(mkw_recompiled)

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "^(Clang|AppleClang)$" OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "WiiCompiled requires a 64-bit Clang toolchain")
endif()

if(APPLE AND CMAKE_OSX_ARCHITECTURES)
    list(LENGTH CMAKE_OSX_ARCHITECTURES MKW_OSX_ARCHITECTURE_COUNT)
    if(MKW_OSX_ARCHITECTURE_COUNT GREATER 1)
        message(FATAL_ERROR
            "WiiCompiled supports one macOS architecture per build directory; "
            "configure separate arm64 and x86_64 build directories")
    endif()
    list(GET CMAKE_OSX_ARCHITECTURES 0 _mkw_osx_arch)
    set(CMAKE_SYSTEM_PROCESSOR "${_mkw_osx_arch}" CACHE STRING "Target processor architecture" FORCE)
    set(CMAKE_SYSTEM_PROCESSOR "${_mkw_osx_arch}")
endif()

if(WIN32 AND MINGW AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64|X86_64)$")
    set(MKW_PLATFORM_WINDOWS TRUE)
elseif(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64|X86_64|arm64|ARM64)$")
    set(MKW_PLATFORM_MACOS TRUE)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64|X86_64)$")
        set(MKW_PLATFORM_MACOS_X86_64 TRUE)
    else()
        set(MKW_PLATFORM_MACOS_ARM64 TRUE)
    endif()
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64|X86_64|aarch64|arm64|ARM64)$")
    set(MKW_PLATFORM_LINUX TRUE)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Android" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
    # Android arm64 through the NDK toolchain file (Meta Quest standalone).
    # Bionic is Linux underneath, so the libco fibers, the mmap guest memory
    # and the POSIX networking all follow the Linux branch; only paths, the
    # SDL entry point and the OpenXR/Vulkan binding are Android-specific.
    set(MKW_PLATFORM_ANDROID TRUE)
else()
    message(FATAL_ERROR
        "WiiCompiled supports 64-bit LLVM-MinGW Clang on Windows, native Linux x86_64/aarch64, Apple Clang on macOS x86_64/arm64, or the Android NDK on arm64-v8a")
endif()

if(MKW_PLATFORM_WINDOWS)
    # CMake and Ninja write response files with Windows quoting, but clang tokenizes them with
    # POSIX rules by default, where a bare apostrophe in a path opens a quoted span.
    add_link_options(--rsp-quoting=windows)
endif()

if(NOT CMAKE_BUILD_TYPE STREQUAL "Release")
    message(FATAL_ERROR "WiiCompiled only supports Release builds")
endif()

option(MKW_BUILD_PRODUCTS "Build translated WiiCompiled product targets" ON)
option(MKW_BUILD_PSQ_TESTS "Build focused PSQ ISA tests" OFF)
set(MKW_X86_CPU_PROFILE "v3" CACHE STRING
    "Minimum x86-64 CPU profile for translated products (v2 or v3)")
set_property(CACHE MKW_X86_CPU_PROFILE PROPERTY STRINGS v2 v3)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64|X86_64)$" AND
   NOT MKW_X86_CPU_PROFILE MATCHES "^(v2|v3)$")
    message(FATAL_ERROR "MKW_X86_CPU_PROFILE must be v2 or v3 (got '${MKW_X86_CPU_PROFILE}')")
endif()

set(MKW_SETUP_VERSION "" CACHE STRING "WiiCompiled version recorded in the runtime log")

option(MKW_PLATFORM_TESTS_ONLY "Build host-platform tests without graphics dependencies or game assets" OFF)
include("${CMAKE_CURRENT_LIST_DIR}/cmake/HostLibraries.cmake")
if(MKW_PLATFORM_TESTS_ONLY)
    if(MKW_BUILD_PRODUCTS)
        message(FATAL_ERROR "MKW_PLATFORM_TESTS_ONLY requires MKW_BUILD_PRODUCTS=OFF")
    endif()
    add_compile_definitions(NOMINMAX)
    include("${CMAKE_CURRENT_LIST_DIR}/cmake/PlatformTests.cmake")
    return()
endif()

# Preprocessor definitions that belong to this project's own code (the runtime,
# the translated shards and the product glue) and to nothing else. They are
# applied directory-scoped, immediately after the aurora add_subdirectory() call,
# so every target created afterwards (mkw_runtime_common, the translated shard
# libraries and the products in cmake/PublicProducts.cmake) inherits them while
# aurora-main and its third-party tree stay unaffected.
set(MKW_PROJECT_COMPILE_DEFINITIONS NOMINMAX)

set(CMAKE_CXX_STANDARD 17)

# Declared before the vendored libraries so the third-party block below can
# branch on it and so cmake/NativePrebuilt.cmake is included after the names it
# must provide are known.
set(MKW_NATIVE_PREBUILT_DIR "" CACHE PATH
    "Precompiled aurora/third-party package to link instead of building aurora-main")
# Maintainer-only counterpart: describe the configured aurora build so the
# package can be harvested from it.
set(MKW_NATIVE_PREBUILT_EXPORT_DIR "" CACHE PATH
    "Maintainer-only: write a description of the configured aurora build here")
option(MKW_USE_PREBUILT_RUNTIME
    "Link the native prebuilt package's runtime archive when it matches this workspace" ON)

# The two Crypto++ configuration macros are load-bearing for ABI: they shape the
# structs and inline code the headers generate, so a first-party TU including
# <cryptopp/...> must see exactly what the archive was compiled with. Both the
# from-source target below and the prebuilt import in cmake/NativePrebuilt.cmake
# consume this one definition.
set(MKW_CRYPTOPP_INTERFACE_DEFINITIONS CRYPTOPP_DISABLE_ASM CRYPTOPP_NO_GLOBAL_BYTE)

# Riivolution XML is parsed with pugixml, matching the parser used by Dolphin.
# The four upstream files from v1.15 (ee86beb30e4973f5feffe3ce63bfa4fbadf72f38)
# are vendored so builds remain usable with FETCHCONTENT_FULLY_DISCONNECTED.
add_library(mkw_pugixml STATIC third_party/pugixml/pugixml.cpp)
add_library(mkw::pugixml ALIAS mkw_pugixml)
target_include_directories(mkw_pugixml PUBLIC third_party/pugixml)
target_compile_features(mkw_pugixml PUBLIC cxx_std_17)
set_target_properties(mkw_pugixml PROPERTIES UNITY_BUILD OFF)

# Runtime configuration is real TOML, parsed by toml11 rather than a project-
# specific line parser. Keep it header-only and vendored so disconnected release
# builds have exactly the same parser as developer builds.
add_library(mkw_toml11 INTERFACE)
add_library(mkw::toml11 ALIAS mkw_toml11)
target_include_directories(mkw_toml11 SYSTEM INTERFACE third_party/toml11)
target_compile_features(mkw_toml11 INTERFACE cxx_std_17)

# Wii ES signatures use the standard binary-field sect233r1 curve. Crypto++
# supplies that curve, SHA-1, ECDSA and an OS-seeded CSPRNG. The complete 8.9.0
# source distribution is vendored. Assembly and algorithm-specific SIMD units
# are disabled to keep the library portable and independent of the runtime's
# x86-64-v3 policy; the static linker retains only the algorithms we reference.
#
# Crypto++ is by far the most expensive vendored library to compile (~150
# translation units), and under the shipped fixed toolchain it is bit-for-bit
# identical for every user, so the prebuilt package harvests its archive and
# this block is skipped entirely in that mode (see cmake/NativePrebuilt.cmake).
if(NOT MKW_NATIVE_PREBUILT_DIR)
    file(GLOB MKW_CRYPTOPP_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_LIST_DIR}/third_party/cryptopp/*.cpp")
    list(FILTER MKW_CRYPTOPP_SOURCES EXCLUDE REGEX
        "/(adhoc|bench[123]|datatest|dlltest|fipsalgt|fipstest|pch|regtest[1234]|simple|test|validat[0-9]+)\\.cpp$")
    list(FILTER MKW_CRYPTOPP_SOURCES EXCLUDE REGEX
        "(_avx|_ppc|_simd|_sse)\\.cpp$")
    if(MKW_PLATFORM_ANDROID)
        # cpu.cpp reads the ARM feature bits through the NDK's cpu-features
        # library, which the NDK ships as source rather than a prebuilt (see the
        # comment above its #include "cpu-features.h"). Compiling it here is the
        # arrangement upstream's own setenv-android.sh sets up.
        set(MKW_ANDROID_CPU_FEATURES_DIR "${CMAKE_ANDROID_NDK}/sources/android/cpufeatures")
        if(NOT EXISTS "${MKW_ANDROID_CPU_FEATURES_DIR}/cpu-features.c")
            message(FATAL_ERROR
                "Crypto++ needs the NDK cpu-features source: ${MKW_ANDROID_CPU_FEATURES_DIR}")
        endif()
        list(APPEND MKW_CRYPTOPP_SOURCES "${MKW_ANDROID_CPU_FEATURES_DIR}/cpu-features.c")
    endif()
    add_library(mkw_cryptopp STATIC ${MKW_CRYPTOPP_SOURCES})
    add_library(mkw::cryptopp ALIAS mkw_cryptopp)
    target_include_directories(mkw_cryptopp SYSTEM PUBLIC third_party)
    if(MKW_PLATFORM_ANDROID)
        target_include_directories(mkw_cryptopp SYSTEM PRIVATE "${MKW_ANDROID_CPU_FEATURES_DIR}")
    endif()
    target_compile_features(mkw_cryptopp PUBLIC cxx_std_17)
    target_compile_definitions(mkw_cryptopp PUBLIC ${MKW_CRYPTOPP_INTERFACE_DEFINITIONS})
    target_compile_options(mkw_cryptopp PRIVATE -w)
    set_target_properties(mkw_cryptopp PROPERTIES UNITY_BUILD OFF)
endif()

# TLS for non-Windows guest network HLE (runtime/src/hle/net/network_ssl.cpp) - the Windows path
# uses Schannel (a Windows-only OS API), which has no equivalent on Linux/Android, so this project
# needs its own TLS library there. mbed TLS was chosen over OpenSSL specifically because it cross-
# compiles cleanly for Android with nothing beyond a plain C toolchain (no perl/asm build-script
# dependency the way OpenSSL's build has), matching how this project already prefers toolchain-
# simple libraries (see Crypto++ above, similarly stripped of ASM/SIMD for portability).
# The from-source build fetches a pinned upstream tarball with a checked SHA-256.
# The Linux native prebuilt package instead ships its compiled archives and headers.
# Bump both values in cmake/MbedTLSPin.cmake when updating; the hash comes from
# upstream's signed `mbedtls-<version>-sha256sum.txt` release asset.
#
# The alias exists on every platform so the link lines in cmake/PublicProducts.cmake stay
# platform-independent, but it is only populated where network_ssl.cpp actually compiles the mbed
# TLS path (`#ifndef _WIN32`). Windows keeps Schannel and must not fetch anything: its builds run
# with FETCHCONTENT_FULLY_DISCONNECTED=ON against the offline dependency set prepared by
# Launcher/Prepare-Dependencies.ps1, so an unconditional fetch would fail a clean configure there
# and would also add a dependency Windows never links.
add_library(mkw_mbedtls INTERFACE)
add_library(mkw::mbedtls ALIAS mkw_mbedtls)
if(NOT MKW_PLATFORM_WINDOWS)
    include("${CMAKE_CURRENT_LIST_DIR}/cmake/MbedTLSPin.cmake")
    if(NOT MKW_NATIVE_PREBUILT_DIR)
        include(FetchContent)
        FetchContent_Declare(mkw_mbedtls_upstream
            URL "https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-${MKW_MBEDTLS_VERSION}/mbedtls-${MKW_MBEDTLS_VERSION}.tar.bz2"
            URL_HASH SHA256=${MKW_MBEDTLS_SHA256}
            DOWNLOAD_EXTRACT_TIMESTAMP FALSE
        )
        # Subproject mode already disables testing and codegen; it still enables programs.
        set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
        set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
        set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
        set(DISABLE_PACKAGE_CONFIG_AND_INSTALL ON CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(mkw_mbedtls_upstream)
        target_link_libraries(mkw_mbedtls INTERFACE MbedTLS::mbedtls MbedTLS::mbedx509 MbedTLS::mbedcrypto)
    endif()
endif()

set(MKW_TRANSLATED_COMPILE_JOBS 0 CACHE STRING
    "Cap on concurrently compiling translated shard TUs via a Ninja job pool (0 = uncapped). \
Scheduling only - never affects output bytes, so it is deliberately outside the canonical flag fingerprint.")
set(MKW_CPPWINRT_INCLUDE_DIR "" CACHE PATH "Optional self-contained C++/WinRT include directory")

# The functional backend is currently Windows D3D12, so only Windows defaults
# to carrying the loader. Linux keeps the capability-gated Vulkan scaffold
# available to explicit developer builds without making a nonfunctional SDK
# fetch part of every normal build.
set(MKW_ENABLE_OPENXR_DEFAULT OFF)
if(MKW_PLATFORM_WINDOWS OR MKW_PLATFORM_ANDROID)
    set(MKW_ENABLE_OPENXR_DEFAULT ON)
endif()
option(MKW_ENABLE_OPENXR "Build OpenXR VR support" ${MKW_ENABLE_OPENXR_DEFAULT})
unset(MKW_ENABLE_OPENXR_DEFAULT)
set(MKW_OPENXR_SDK_DIR "" CACHE PATH
    "Optional OpenXR-SDK source tree; when empty, use a package or fetch the pinned SDK")

# Precompiled aurora + third-party package (see launcher/Prepare-NativePrebuilt.ps1).
# Empty - the default - keeps the from-source behaviour every developer build
# uses. When set, aurora-main, its extern/ tree and the vendored Crypto++ are
# not compiled at all; imported targets with the same names are wired up from
# the package instead, so this project's own code compiles and links exactly
# as before.
set(MKW_AURORA_DIR "${CMAKE_CURRENT_LIST_DIR}/../aurora-main")

# The translator's explicit float casts are PPC single-precision rounding points.
# Fast-math may erase them and change guest-visible integer conversions.
set(MKW_TRANSLATED_PPC_FP_OPTIONS -fno-fast-math -ffp-contract=off)

set(MKW_PPC_SEMANTIC_RUNTIME_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/src/ppc_helpers.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/fpu_helpers.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/ppc_quantized.cpp"
    # Native reimplementations of the game's culling math (VR object culling).
    "${CMAKE_CURRENT_LIST_DIR}/src/vr/mkw_vr_culling.cpp")
set_source_files_properties(${MKW_PPC_SEMANTIC_RUNTIME_SOURCES} PROPERTIES
    SKIP_UNITY_BUILD_INCLUSION ON
    SKIP_PRECOMPILE_HEADERS ON
    COMPILE_OPTIONS "${MKW_TRANSLATED_PPC_FP_OPTIONS}")
# The first-person cockpit and the character body it includes
# (character_body_ik.inl) reject NaN/Inf guest matrices and tracked poses
# before posing anything with them. -ffinite-math-only, part of the runtime's
# -ffast-math, folds such a check away once the value has been through any
# arithmetic, so this one file opts back out of that assumption (see
# frame_interpolation.cpp below for the same reason).
set_source_files_properties("${CMAKE_CURRENT_LIST_DIR}/src/vr/mkw_vr_first_person.cpp" PROPERTIES
    SKIP_UNITY_BUILD_INCLUSION ON
    SKIP_PRECOMPILE_HEADERS ON
    COMPILE_OPTIONS "-fno-finite-math-only")
# Match the optimization policy of the translated callers as well as their FP policy.
set_property(SOURCE "${CMAKE_CURRENT_LIST_DIR}/src/ppc_quantized.cpp" APPEND PROPERTY
    COMPILE_OPTIONS -O2 -fno-slp-vectorize)

# ----------------------------------------------------------------------
# Third-party: aurora-main (provides SDL3 + GPU backends)
# ----------------------------------------------------------------------
if(MKW_NATIVE_PREBUILT_DIR)
    # Precompiled aurora + third-party archives. The source trees are still
    # present (the runtime includes aurora headers directly), they are simply
    # not compiled here.
    include("${CMAKE_CURRENT_LIST_DIR}/cmake/NativePrebuilt.cmake")
else()
    if(NOT EXISTS ${MKW_AURORA_DIR}/CMakeLists.txt)
        message(FATAL_ERROR "Requested aurora-main but ${MKW_AURORA_DIR} is missing")
    endif()
    if(MKW_PLATFORM_ANDROID)
        # encounter/dawn-build publishes the same release for android-aarch64;
        # the provider pins its digest and maps the asset name. SDL3 is built
        # from source as a shared library because SDLActivity loads libSDL3.so
        # before libmain.so; a Gradle build may instead pass
        # -DAURORA_SDL3_PROVIDER=system to use the prefab from the SDL3 AAR.
        set(AURORA_DAWN_PROVIDER package CACHE STRING "" FORCE)
        set(AURORA_SDL3_LINKAGE shared CACHE STRING "" FORCE)
        if(NOT AURORA_SDL3_PROVIDER OR AURORA_SDL3_PROVIDER STREQUAL "auto")
            set(AURORA_SDL3_PROVIDER vendor CACHE STRING "" FORCE)
        endif()
    endif()
    set(DAWN_ENABLE_D3D11 OFF CACHE BOOL "" FORCE)
    if(MKW_PLATFORM_WINDOWS)
        set(DAWN_ENABLE_D3D12 ON CACHE BOOL "" FORCE)
        set(DAWN_ENABLE_VULKAN ON CACHE BOOL "" FORCE)
        set(DAWN_ENABLE_METAL OFF CACHE BOOL "" FORCE)
        set(TINT_BUILD_HLSL_WRITER ON CACHE BOOL "" FORCE)
    elseif(MKW_PLATFORM_MACOS)
        set(DAWN_ENABLE_D3D12 OFF CACHE BOOL "" FORCE)
        set(DAWN_ENABLE_VULKAN OFF CACHE BOOL "" FORCE)
        set(DAWN_ENABLE_METAL ON CACHE BOOL "" FORCE)
        set(TINT_BUILD_HLSL_WRITER OFF CACHE BOOL "" FORCE)
    else()
        set(DAWN_ENABLE_D3D12 OFF CACHE BOOL "" FORCE)
        set(DAWN_ENABLE_VULKAN ON CACHE BOOL "" FORCE)
        set(DAWN_ENABLE_METAL OFF CACHE BOOL "" FORCE)
        set(TINT_BUILD_HLSL_WRITER OFF CACHE BOOL "" FORCE)
    endif()
    set(DAWN_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
    set(DAWN_BUILD_TESTS OFF CACHE BOOL "" FORCE)

    # Provide a tiny stub for DXProgrammableCapture when the SDK/PIX headers are
    # missing (common on MinGW). Dawn only includes the header; no symbols are
    # referenced when PIX isn't present.
    if(MKW_PLATFORM_WINDOWS)
        set(MKW_DX_STUB_DIR "${CMAKE_BINARY_DIR}/aurora_dx_stubs")
        if(NOT EXISTS "${MKW_DX_STUB_DIR}/DXProgrammableCapture.h")
            file(MAKE_DIRECTORY ${MKW_DX_STUB_DIR})
            file(WRITE "${MKW_DX_STUB_DIR}/DXProgrammableCapture.h"
"#pragma once\n// Stubbed PIX capture header for Dawn; no functionality when PIX isn't present.\n")
        endif()
    endif()
    # Deliberately NOT injected project-wide. Only a from-source Dawn build ever
    # includes DXProgrammableCapture.h, and this tree consumes Dawn as a prebuilt
    # package (AURORA_DAWN_PROVIDER=package), so the old
    # include_directories()/add_compile_options(-I...) pair only added a dead
    # -I to every one of the ~500 third-party and runtime compile lines. The
    # aurora_* targets that do want it get it from the
    # target_include_directories() call further down; a from-source Dawn build
    # should add it to the Dawn targets the same way.

    # Keep SDL3 lean when aurora is enabled.
    if(MKW_PLATFORM_ANDROID)
        set(SDL_SHARED ON CACHE BOOL "" FORCE)
        set(SDL_STATIC OFF CACHE BOOL "" FORCE)
    else()
        set(SDL_SHARED OFF CACHE BOOL "" FORCE)
        set(SDL_STATIC ON CACHE BOOL "" FORCE)
    endif()
    set(SDL_TEST OFF CACHE BOOL "" FORCE)

    # MinGW/WSL can choke on SDL precompiled headers; disable PCH for aurora/SDL.
    set(CMAKE_DISABLE_PRECOMPILE_HEADERS ON)
    # Isolate aurora build products to a subdirectory under the main build dir.
    add_subdirectory(${MKW_AURORA_DIR} ${CMAKE_BINARY_DIR}/aurora-build EXCLUDE_FROM_ALL)
    set(CMAKE_DISABLE_PRECOMPILE_HEADERS OFF)

    # Keep upstream Aurora split: several sources intentionally use file-local
    # names that collide under this repo's aggressive unity build settings.
    set(AURORA_TARGETS aurora_core aurora_gx aurora_pad aurora_si aurora_mtx aurora_vi)
    foreach(t ${AURORA_TARGETS})
        if(TARGET ${t})
            set_target_properties(${t} PROPERTIES UNITY_BUILD OFF)
            target_compile_options(${t} PRIVATE -O3 -ffast-math -w -pipe)
            if(MKW_PLATFORM_WINDOWS)
                target_include_directories(${t} PRIVATE ${MKW_DX_STUB_DIR})
            endif()
            # Aurora's own sources include Windows headers and call std::min/max;
            # they relied on the old project-wide NOMINMAX that no longer leaks
            # into this subtree, so the define is applied per target here.
            target_compile_definitions(${t} PRIVATE NOMINMAX)
        endif()
    endforeach()

    # Frame interpolation filters guest NaN/Inf matrices out of a std::sort
    # comparator, which makes its std::isfinite guards load-bearing for memory
    # safety, not just match quality. -ffinite-math-only (part of -ffast-math
    # above) folds those guards to constants and poisons NaN float arguments
    # via nofpclass, so this one file opts back out of the finite-math
    # assumption; the file #errors if built without this. Source-scoped
    # COMPILE_OPTIONS append after the target options, so this overrides the
    # target's -ffast-math for exactly this TU.
    if(TARGET aurora_gx)
        set_source_files_properties("${MKW_AURORA_DIR}/lib/gx/frame_interpolation.cpp"
            TARGET_DIRECTORY aurora_gx
            PROPERTIES COMPILE_OPTIONS "-fno-finite-math-only")
    endif()
endif()

# Everything from here down is this project's own code, so this is where the
# project definitions enter the directory scope. A directory-scoped definition
# seeds the COMPILE_DEFINITIONS of targets as they are created, which means:
#   * aurora-main and its third-party tree were added above -> unaffected;
#   * the runtime library, the translated shard libraries and the products in
#     cmake/RuntimeLibrary.cmake and cmake/PublicProducts.cmake are all created
#     below (those files are include()d, not add_subdirectory()d, so they share
#     this directory scope) -> they pick the definitions up exactly as before.
if(MKW_PROJECT_COMPILE_DEFINITIONS)
    add_compile_definitions(${MKW_PROJECT_COMPILE_DEFINITIONS})
endif()

if(MKW_PLATFORM_WINDOWS AND MKW_ENABLE_OPENXR)
    include(FetchContent)
    FetchContent_Declare(vulkan_headers
        URL "https://github.com/KhronosGroup/Vulkan-Headers/archive/015e25c3c91b70eb1a754d36fb14c4ba6ad9b0b9.tar.gz"
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(vulkan_headers)
    include_directories("${vulkan_headers_SOURCE_DIR}/include")
endif()

set(MKW_OPENXR_TARGET "")
if(MKW_ENABLE_OPENXR)
    if(NOT MKW_PLATFORM_WINDOWS AND NOT MKW_PLATFORM_LINUX AND NOT MKW_PLATFORM_ANDROID)
        message(WARNING "OpenXR is not wired for this platform; disabling MKW_ENABLE_OPENXR")
        set(MKW_ENABLE_OPENXR OFF)
    else()
        # An installer-supplied SDK must win over any developer machine package.
        if(NOT MKW_OPENXR_SDK_DIR AND NOT FETCHCONTENT_SOURCE_DIR_OPENXR)
            find_package(OpenXR CONFIG QUIET)
        endif()
        if(NOT TARGET OpenXR::openxr_loader AND NOT TARGET openxr_loader)
            include(FetchContent)
            set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
            set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
            set(BUILD_CONFORMANCE_TESTS OFF CACHE BOOL "" FORCE)
            set(BUILD_SDK_TESTS OFF CACHE BOOL "" FORCE)
            set(DYNAMIC_LOADER OFF CACHE BOOL "" FORCE)
            if(MKW_OPENXR_SDK_DIR)
                if(NOT EXISTS "${MKW_OPENXR_SDK_DIR}/CMakeLists.txt")
                    message(FATAL_ERROR
                        "MKW_OPENXR_SDK_DIR does not contain OpenXR-SDK: ${MKW_OPENXR_SDK_DIR}")
                endif()
                add_subdirectory("${MKW_OPENXR_SDK_DIR}"
                    "${CMAKE_BINARY_DIR}/openxr-sdk" EXCLUDE_FROM_ALL)
            else()
                # Keep the FetchContent name aligned with the installer's
                # BuildWorkspace/Dependencies/openxr directory. The bundled
                # build passes FETCHCONTENT_SOURCE_DIR_OPENXR and runs fully
                # disconnected, while developer builds may fetch this pin.
                FetchContent_Declare(openxr
                    URL https://github.com/KhronosGroup/OpenXR-SDK/archive/refs/tags/release-1.1.61.tar.gz
                    URL_HASH SHA256=d5e2773d282642e6e250bd10266f316fc3ff3b8602c83db009a1f434205998e6
                    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
                    EXCLUDE_FROM_ALL)
                FetchContent_MakeAvailable(openxr)
            endif()
        endif()
        if(TARGET OpenXR::openxr_loader)
            set(MKW_OPENXR_TARGET OpenXR::openxr_loader)
        elseif(TARGET openxr_loader)
            set(MKW_OPENXR_TARGET openxr_loader)
        else()
            message(FATAL_ERROR "OpenXR was enabled but no loader target was provided")
        endif()
        add_compile_definitions(MKW_ENABLE_OPENXR=1)
    endif()
endif()

# Runtime sources. CONFIGURE_DEPENDS costs one glob re-check per build (measured
# at ~55 ms across the 100+ globs this build already re-verifies for SDL), and it
# buys correctness: without it, a newly added src/*.cpp that is not referenced by
# a registration file is silently never compiled and never errors. The stale-glob
# failure mode is worth far more than the milliseconds.
file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS "src/*.cpp")
set_source_files_properties(src/vr/openxr_vulkan_win32.cpp PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)
set_source_files_properties(src/vr/openxr_vulkan_direct.cpp PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)
if(MKW_PLATFORM_MACOS)
    list(REMOVE_ITEM SOURCES "${CMAKE_CURRENT_LIST_DIR}/src/guest_flat_memory.cpp")
    if(MKW_PLATFORM_MACOS_ARM64)
        # HostContext's Apple Silicon backend is implemented in a small assembly
        # companion. It must be part of the product runtime as well as the
        # standalone context test; otherwise the final executable is missing
        # mkw_co_init/mkw_co_switch at link time.
        enable_language(ASM)
        list(APPEND SOURCES "${CMAKE_CURRENT_LIST_DIR}/src/platform/macos/co_switch.S")
    endif()
else()
    list(REMOVE_ITEM SOURCES "${CMAKE_CURRENT_LIST_DIR}/src/guest_flat_memory_macos.cpp")
endif()
set(MKW_PLATFORM_SOURCE "${CMAKE_CURRENT_LIST_DIR}/src/platform/host_platform.cpp")
set(MKW_BASE_PRODUCT_SOURCE "${CMAKE_CURRENT_LIST_DIR}/src/product/base_product.cpp")
set(MKW_RETRO_REWIND_PRODUCT_SOURCE "${CMAKE_CURRENT_LIST_DIR}/src/product/retro_rewind_product.cpp")
# The host ISA guard is the one translation unit that must not receive the
# x86-64-v3 flags, so it gets its own object library instead of riding along in
# the runtime library. See cmake/PublicProducts.cmake and the file's own header.
set(MKW_CPU_BASELINE_SOURCE "${CMAKE_CURRENT_LIST_DIR}/src/host_cpu_baseline.cpp")
list(REMOVE_ITEM SOURCES ${MKW_BASE_PRODUCT_SOURCE} ${MKW_RETRO_REWIND_PRODUCT_SOURCE}
     ${MKW_CPU_BASELINE_SOURCE} ${MKW_PLATFORM_SOURCE})

# Keep these independent from Aurora's BUILD_TESTING option: they validate the
# project's host-platform contracts, not Aurora's third-party test suite.
# A cross-compiled Android tree cannot run them, so it does not build them.
if(MKW_PLATFORM_ANDROID)
    set(MKW_BUILD_TESTS_DEFAULT OFF)
else()
    set(MKW_BUILD_TESTS_DEFAULT ON)
endif()
option(MKW_BUILD_TESTS "Build the runtime host-contract test executables" ${MKW_BUILD_TESTS_DEFAULT})
unset(MKW_BUILD_TESTS_DEFAULT)
if(MKW_BUILD_TESTS)
    include("${CMAKE_CURRENT_LIST_DIR}/cmake/PlatformTests.cmake")
    # The VR camera, input, policy and OpenXR contracts; they need the vendored
    # toml11 and the OpenXR loader target configured above.
    include("${CMAKE_CURRENT_LIST_DIR}/cmake/VrTests.cmake")
endif()

set(MKW_RUNTIME_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}")
include("${CMAKE_CURRENT_LIST_DIR}/cmake/RuntimeLibrary.cmake")

# The prebuilt export also carries the game-independent runtime as a static
# archive (x86_64 only; AArch64 compiles it locally with -mcpu=native). It is
# defined here, after the project definitions, so its flags match the products'.
if(MKW_NATIVE_PREBUILT_EXPORT_DIR AND NOT MKW_NATIVE_PREBUILT_DIR)
    if(MKW_HOST_IS_X86_64)
        mkw_add_runtime_library(mkw_runtime STATIC ${SOURCES})
        set_target_properties(mkw_runtime PROPERTIES EXCLUDE_FROM_ALL ON)
    endif()
    include("${CMAKE_CURRENT_LIST_DIR}/cmake/NativePrebuiltExport.cmake")
endif()

# The translator emits the complete, content-addressed source graph. Consuming
# this one manifest keeps configure independent of the 28k generated function
# files and of optional Retro Rewind artifacts such as code.map.
if(MKW_BUILD_PRODUCTS)
    set(MKW_TRANSLATED_SHARD_MANIFEST
        "${CMAKE_CURRENT_LIST_DIR}/../generated/build_shards/shards.cmake"
        CACHE FILEPATH "Translator-owned aggregate shard manifest")
    # The prebuilt export only needs the aurora/third-party closure and the runtime
    # configured above, so a packaging machine without a translation stops here.
    if(MKW_NATIVE_PREBUILT_EXPORT_DIR AND NOT EXISTS "${MKW_TRANSLATED_SHARD_MANIFEST}")
        message(STATUS "No translator shard manifest; configuring the native prebuilt export only")
        return()
    endif()
    if(NOT EXISTS "${MKW_TRANSLATED_SHARD_MANIFEST}")
        message(FATAL_ERROR
            "Missing translator-owned shard manifest: ${MKW_TRANSLATED_SHARD_MANIFEST}. "
            "Run Translator.Cli emit-build-shards first; see translator/README.md.")
    endif()
    include("${MKW_TRANSLATED_SHARD_MANIFEST}")
    set(MKW_HAVE_RETRO_REWIND ${MKW_HAVE_RETRO_REWIND_SHARDS})
    message(STATUS
        "Translator graph: ${MKW_SHARED_BASE_FUNCTION_COUNT}/${MKW_BASE_FUNCTION_COUNT} base functions shared; "
        "${MKW_PROFILE_SENSITIVE_CALLER_COUNT} profile-sensitive callers; "
        "${MKW_RETRO_REWIND_FUNCTION_COUNT} Retro Rewind functions")

    include("${CMAKE_CURRENT_LIST_DIR}/cmake/PublicProducts.cmake")
else()
    if(MKW_PLATFORM_MACOS)
        # Compile-only audit of native runtime sources. It deliberately avoids
        # translated products until their host dependencies are portable.
        # runtime/src is game-independent; per-game glue lives in runtime/game.
        add_library(mkw_macos_native_compile OBJECT ${SOURCES})
        target_include_directories(mkw_macos_native_compile PRIVATE
            "${CMAKE_CURRENT_LIST_DIR}/include" "${CMAKE_CURRENT_LIST_DIR}/src"
            "${CMAKE_CURRENT_LIST_DIR}/.." "${CMAKE_CURRENT_LIST_DIR}/../aurora-main/include")
        target_compile_features(mkw_macos_native_compile PRIVATE cxx_std_20)
        target_compile_definitions(mkw_macos_native_compile PRIVATE SDL_MAIN_HANDLED TARGET_PC)
        target_link_libraries(mkw_macos_native_compile PRIVATE
            aurora::gx aurora::pad aurora::si aurora::vi aurora::mtx
            mkw::pugixml mkw::toml11 mkw::cryptopp mkw::mbedtls)
        if(MKW_PLATFORM_MACOS_X86_64)
            target_link_libraries(mkw_macos_native_compile PRIVATE mkw::libco)
            target_compile_options(mkw_macos_native_compile PRIVATE
                -march=x86-64-${MKW_X86_CPU_PROFILE})
            if(MKW_X86_CPU_PROFILE STREQUAL "v2")
                target_compile_definitions(mkw_macos_native_compile PRIVATE MKW_X86_CPU_PROFILE_V2=1)
            endif()
        endif()
        set_target_properties(mkw_macos_native_compile PROPERTIES UNITY_BUILD OFF)
    endif()
    if(MKW_PLATFORM_ANDROID)
        # Compile-only audit of the native runtime for arm64 Android, including
        # the OpenXR Vulkan backend and the Android platform glue, without a
        # translated game. Same exclusions as the macOS audit: those two files
        # include the translator-emitted RuntimeConfig.h.
        set(MKW_ANDROID_NATIVE_AUDIT_SOURCES ${SOURCES})
        list(REMOVE_ITEM MKW_ANDROID_NATIVE_AUDIT_SOURCES
            "${CMAKE_CURRENT_LIST_DIR}/src/abi_bridge.cpp"
            "${CMAKE_CURRENT_LIST_DIR}/src/hle/os/os_alarm.cpp")
        add_library(mkw_android_native_compile OBJECT ${MKW_ANDROID_NATIVE_AUDIT_SOURCES})
        target_include_directories(mkw_android_native_compile PRIVATE
            "${CMAKE_CURRENT_LIST_DIR}/include" "${CMAKE_CURRENT_LIST_DIR}/src"
            "${CMAKE_CURRENT_LIST_DIR}/.." "${CMAKE_CURRENT_LIST_DIR}/../aurora-main/include")
        target_compile_features(mkw_android_native_compile PRIVATE cxx_std_20)
        target_compile_definitions(mkw_android_native_compile PRIVATE TARGET_PC)
        target_link_libraries(mkw_android_native_compile PRIVATE
            aurora::gx aurora::pad aurora::si aurora::vi aurora::mtx
            mkw::pugixml mkw::toml11 mkw::cryptopp mkw::libco)
        if(MKW_ENABLE_OPENXR)
            target_link_libraries(mkw_android_native_compile PRIVATE ${MKW_OPENXR_TARGET})
        endif()
        # Same precompiled header as the product objects: several sources rely on
        # it for abi_bridge.h / ppc_runtime.h rather than including them directly.
        target_precompile_headers(mkw_android_native_compile PRIVATE "${CMAKE_CURRENT_LIST_DIR}/include/mkw_pch.h")
        set_target_properties(mkw_android_native_compile PROPERTIES UNITY_BUILD OFF)
    endif()
    add_custom_target(mkw_platform_paths_check DEPENDS mkw_platform)
    message(STATUS "Translated product targets disabled (MKW_BUILD_PRODUCTS=OFF)")
endif()
