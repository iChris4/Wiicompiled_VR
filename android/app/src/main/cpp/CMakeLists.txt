cmake_minimum_required(VERSION 3.22)
project(wiicompiled_quest LANGUAGES C CXX ASM)

# Gradle drives this file. It hands the repository's runtime/ tree to CMake
# with the Android-specific choices the desktop build never needs:
#   * SDL3 comes from the AAR's prefab module (SDLActivity must load the same
#     libSDL3.so the Java classes were built against);
#   * the products are shared libraries (see runtime/cmake/PublicProducts.cmake);
#   * the translated graph is the one the Windows installer generated, whose
#     blob assembly PublicProducts.cmake rewrites for ELF.

if(NOT ANDROID OR NOT CMAKE_ANDROID_ARCH_ABI STREQUAL "arm64-v8a")
    message(FATAL_ERROR "The Quest build supports Android arm64-v8a only")
endif()
if(NOT DEFINED MKW_REPO_ROOT OR NOT EXISTS "${MKW_REPO_ROOT}/runtime/CMakeLists.txt")
    message(FATAL_ERROR "MKW_REPO_ROOT must name the WiiCompiled checkout")
endif()

# Translated PowerPC is Release-only by policy (see runtime/CMakeLists.txt);
# a debug-signed APK still carries a release-compiled game.
set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)

# Reuse already-fetched dependency sources when the caller has them (the
# installer's BuildWorkspace/Dependencies); FetchContent honours these
# per-dependency overrides and skips the download.
if(DEFINED MKW_DEPENDENCIES_DIR AND IS_DIRECTORY "${MKW_DEPENDENCIES_DIR}")
    file(GLOB _mkw_dependency_dirs LIST_DIRECTORIES true "${MKW_DEPENDENCIES_DIR}/*")
    foreach(_dir IN LISTS _mkw_dependency_dirs)
        if(IS_DIRECTORY "${_dir}")
            get_filename_component(_name "${_dir}" NAME)
            if(NOT _name MATCHES "^(dawn_prebuilt|native_prebuilt|cppwinrt|libusb)$")
                string(TOUPPER "${_name}" _upper)
                set("FETCHCONTENT_SOURCE_DIR_${_upper}" "${_dir}" CACHE PATH "" FORCE)
            endif()
        endif()
    endforeach()
endif()

# The Dawn package android/Build-QuestDawn.ps1 built with Aurora's patches (fragment density maps for
# foveated rendering) stands in for the stock prebuilt one. The override lives in this build
# directory's cache, so a build without the package clears it rather than keep the last one.
if(DEFINED MKW_QUEST_DAWN_PACKAGE_DIR AND NOT MKW_QUEST_DAWN_PACKAGE_DIR STREQUAL "")
    if(NOT EXISTS "${MKW_QUEST_DAWN_PACKAGE_DIR}/aurora-dawn.json")
        message(FATAL_ERROR "No patched Dawn package at ${MKW_QUEST_DAWN_PACKAGE_DIR}; run android/Build-QuestDawn.ps1")
    endif()
    set(FETCHCONTENT_SOURCE_DIR_DAWN_PREBUILT "${MKW_QUEST_DAWN_PACKAGE_DIR}" CACHE PATH "" FORCE)
else()
    unset(FETCHCONTENT_SOURCE_DIR_DAWN_PREBUILT CACHE)
endif()

find_package(SDL3 REQUIRED CONFIG)
set(AURORA_SDL3_PROVIDER system CACHE STRING "" FORCE)
set(AURORA_SDL3_LINKAGE shared CACHE STRING "" FORCE)
set(MKW_ENABLE_OPENXR ON CACHE BOOL "" FORCE)
set(MKW_BUILD_PRODUCTS ON CACHE BOOL "" FORCE)
set(MKW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
if(DEFINED MKW_GENERATED_DIR)
    set(MKW_TRANSLATED_SHARD_MANIFEST "${MKW_GENERATED_DIR}/build_shards/shards.cmake"
        CACHE FILEPATH "" FORCE)
endif()

add_subdirectory("${MKW_REPO_ROOT}/runtime" wiicompiled-runtime)

# App-owned JNI glue lives in each product library SDLActivity loads, and in the game kit
# probe (runtime/cmake/PublicProducts.cmake) whose objects the player's game is linked with.
foreach(product IN ITEMS WiiCompiled RetroRewind mkw_quest_kit_probe mkw_quest_kit_probe_retro)
    if(TARGET ${product})
        target_sources(${product} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/quest_surface_jni.cpp")
        target_include_directories(${product} PRIVATE "${MKW_REPO_ROOT}/aurora-main/include")
    endif()
endforeach()
