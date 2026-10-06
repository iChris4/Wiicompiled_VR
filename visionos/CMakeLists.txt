# WiiCompiled for Apple Vision Pro: the SwiftUI app around the game runtime.
#
# One Xcode project (the only generator that signs, embeds Swift and produces a
# visionOS bundle) holding the runtime tree (../runtime, which on visionOS
# yields the products as frameworks) and this app, which embeds the products
# (the base game, and Retro Rewind when translated), loads the chosen one with
# dlopen when the player presses Play and drives it through
# runtime/include/platform/visionos/visionos_host.h.
#
#   cmake -S visionos -B build-visionos -G Xcode \
#       -DCMAKE_SYSTEM_NAME=visionOS -DCMAKE_OSX_SYSROOT=xros \
#       -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=2.0 \
#       -DAURORA_DAWN_PACKAGE_URL=file:///.../dawn-visionos-arm64.tar.gz \
#       -DMKW_VISIONOS_TEAM=<Apple developer team id>
#   cmake --build build-visionos --config Release --target WiiCompiledVision
#
# visionos/Build-VisionOS.sh runs both, after building Dawn for the SDK
# (Build-VisionOSDawn.sh). The translation (generated/) must exist already, as
# for any other platform: docs/building-macos.md, steps 1 to 5.
cmake_minimum_required(VERSION 3.28)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "visionOS")
    message(FATAL_ERROR "Configure with -DCMAKE_SYSTEM_NAME=visionOS (and -DCMAKE_OSX_SYSROOT=xros or xrsimulator)")
endif()
if(NOT CMAKE_GENERATOR STREQUAL "Xcode")
    message(FATAL_ERROR "The visionOS app needs the Xcode generator (-G Xcode): Swift, signing and the bundle come from it")
endif()
if(NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES arm64 CACHE STRING "" FORCE)
endif()
if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET 2.0 CACHE STRING "" FORCE)
endif()
# The runtime only builds Release; Xcode is multi-config, so it is the one configuration.
set(CMAKE_CONFIGURATION_TYPES Release CACHE STRING "" FORCE)
set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)

project(WiiCompiledVision LANGUAGES C CXX OBJC OBJCXX Swift ASM)

set(MKW_VISIONOS_RETRO_REWIND AUTO CACHE STRING "Carry Retro Rewind next to the base game: AUTO (when the translation has it), ON or OFF")
set_property(CACHE MKW_VISIONOS_RETRO_REWIND PROPERTY STRINGS AUTO ON OFF)
set(MKW_VISIONOS_RETRO_REWIND_ROOT "" CACHE PATH "The RetroRewind6 folder the mod was translated from; its version.txt tells the launcher which pack to install")
set(MKW_VISIONOS_TEAM "" CACHE STRING "Apple developer team id for automatic signing (empty: sign in Xcode)")
set(MKW_VISIONOS_BUNDLE_ID "" CACHE STRING "Bundle identifier of the app (empty: org.wiicompiled.vision.<team id>)")
# org.wiicompiled.vision is registered to the project's own team, and an App ID belongs to one
# team only, so the identifier defaults to one made from the signing team's id, which that team
# can register. A cache from before this default keeps the identifier it holds, and changing it
# later installs a second, separate app.
set(MKW_VISIONOS_APP_BUNDLE_ID "${MKW_VISIONOS_BUNDLE_ID}")
if(NOT MKW_VISIONOS_APP_BUNDLE_ID)
    if(MKW_VISIONOS_TEAM)
        string(TOLOWER "org.wiicompiled.vision.${MKW_VISIONOS_TEAM}" MKW_VISIONOS_APP_BUNDLE_ID)
    else()
        set(MKW_VISIONOS_APP_BUNDLE_ID "org.wiicompiled.vision")
    endif()
endif()
set(MKW_VISIONOS_APP_NAME "WiiCompiled Vision" CACHE STRING "Name shown on the Home View")

# Xcode signs every Mach-O it produces on the iOS family, including the dependency
# libraries' by-products (libpng's dylib, for one), and refuses without a team. Only the
# app bundle is signed; the libraries are linked into it and need no identity of their own.
set(CMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED NO)
set(CMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED NO)
set(CMAKE_XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "")
# Only the game framework is loaded dynamically; skip the shared flavours the dependencies offer.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(PNG_SHARED OFF CACHE BOOL "" FORCE)
set(PNG_TOOLS OFF CACHE BOOL "" FORCE)
set(PNG_TESTS OFF CACHE BOOL "" FORCE)
set(PNG_FRAMEWORK OFF CACHE BOOL "" FORCE)

# The runtime tree. Its own CMakeLists detects visionOS from CMAKE_SYSTEM_NAME,
# turns OpenXR on with the CompositorServices provider and makes the products
# frameworks (runtime/cmake/PublicProducts.cmake).
set(MKW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../runtime" runtime)

# The games the app carries, one framework each; the launcher's game picker loads the chosen
# one at Play. Retro Rewind exists when the translation ran with the mod (docs/building-macos.md,
# step C, gives generated/build_shards a RetroRewind target).
set(MKW_VISIONOS_GAME_TARGETS WiiCompiled)
if(MKW_VISIONOS_RETRO_REWIND STREQUAL "AUTO")
    if(TARGET RetroRewind)
        set(MKW_VISIONOS_WITH_RETRO_REWIND ON)
    else()
        set(MKW_VISIONOS_WITH_RETRO_REWIND OFF)
    endif()
elseif(MKW_VISIONOS_RETRO_REWIND)
    if(NOT TARGET RetroRewind)
        message(FATAL_ERROR "MKW_VISIONOS_RETRO_REWIND=ON but the translation has no Retro Rewind shards")
    endif()
    set(MKW_VISIONOS_WITH_RETRO_REWIND ON)
else()
    set(MKW_VISIONOS_WITH_RETRO_REWIND OFF)
endif()

# The translated mod is bound to one Code.pul (the translator refuses to reuse a base translation
# made without it), and the headset cannot retranslate the way the Quest and PC launchers rebuild.
# So the app records which pack it was made from: the launcher installs that version of the pack
# and refuses a pack whose Code.pul differs. The hash comes from the copy the translator consumed;
# the version from the pack folder when the configure names it.
set(MKW_VISIONOS_RR_AVAILABLE_SWIFT false)
set(MKW_VISIONOS_RR_VERSION "")
set(MKW_VISIONOS_RR_CODE_PUL_SHA256 "")
if(MKW_VISIONOS_WITH_RETRO_REWIND)
    list(APPEND MKW_VISIONOS_GAME_TARGETS RetroRewind)
    set(MKW_VISIONOS_RR_AVAILABLE_SWIFT true)
    set(_rr_code_pul "${CMAKE_CURRENT_SOURCE_DIR}/../PulsarPacks/completed/RetroRewind/RetroRewind6/Binaries/Code.pul")
    if(MKW_VISIONOS_RETRO_REWIND_ROOT)
        if(EXISTS "${MKW_VISIONOS_RETRO_REWIND_ROOT}/Binaries/Code.pul")
            set(_rr_code_pul "${MKW_VISIONOS_RETRO_REWIND_ROOT}/Binaries/Code.pul")
        endif()
        if(EXISTS "${MKW_VISIONOS_RETRO_REWIND_ROOT}/version.txt")
            file(STRINGS "${MKW_VISIONOS_RETRO_REWIND_ROOT}/version.txt" _rr_version_lines LIMIT_COUNT 1)
            list(GET _rr_version_lines 0 MKW_VISIONOS_RR_VERSION)
            string(STRIP "${MKW_VISIONOS_RR_VERSION}" MKW_VISIONOS_RR_VERSION)
        endif()
    endif()
    if(EXISTS "${_rr_code_pul}")
        file(SHA256 "${_rr_code_pul}" MKW_VISIONOS_RR_CODE_PUL_SHA256)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_rr_code_pul}")
    else()
        message(WARNING "No Code.pul at ${_rr_code_pul}: the launcher cannot check the installed pack against this build")
    endif()
    if(NOT MKW_VISIONOS_RR_VERSION)
        message(STATUS "Retro Rewind pack version unknown (set MKW_VISIONOS_RETRO_REWIND_ROOT); the launcher will install the newest pack")
    endif()
endif()

set(MKW_VISIONOS_APP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/App")
set(MKW_VISIONOS_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated-sources")
configure_file("${MKW_VISIONOS_APP_DIR}/RetroRewindBuild.swift.in" "${MKW_VISIONOS_GENERATED_DIR}/RetroRewindBuild.swift" @ONLY)
set(MKW_VISIONOS_APP_SOURCES
    "${MKW_VISIONOS_APP_DIR}/WiiCompiledVisionApp.swift"
    "${MKW_VISIONOS_APP_DIR}/GameModel.swift"
    "${MKW_VISIONOS_APP_DIR}/LauncherView.swift"
    "${MKW_VISIONOS_APP_DIR}/ImmersiveGameView.swift"
    "${MKW_VISIONOS_APP_DIR}/GameLibrary.swift"
    "${MKW_VISIONOS_APP_DIR}/TomlConfig.swift"
    "${MKW_VISIONOS_APP_DIR}/GameStorage.swift"
    "${MKW_VISIONOS_APP_DIR}/SettingsView.swift"
    "${MKW_VISIONOS_APP_DIR}/RetroRewindPack.swift"
    "${MKW_VISIONOS_APP_DIR}/ZipArchive.swift"
    "${MKW_VISIONOS_APP_DIR}/Mii.swift"
    "${MKW_VISIONOS_APP_DIR}/MiiDatabase.swift"
    "${MKW_VISIONOS_APP_DIR}/FflResource.swift"
    "${MKW_VISIONOS_APP_DIR}/MiiBodies.swift"
    "${MKW_VISIONOS_APP_DIR}/MiiRenderer.swift"
    "${MKW_VISIONOS_APP_DIR}/MiiRenderResource.swift"
    "${MKW_VISIONOS_APP_DIR}/MiiImages.swift"
    "${MKW_VISIONOS_APP_DIR}/MiisView.swift"
    "${MKW_VISIONOS_APP_DIR}/MiiEditorView.swift"
    "${MKW_VISIONOS_APP_DIR}/RksysProfiles.swift"
    "${MKW_VISIONOS_APP_DIR}/RetroWfc.swift"
    "${MKW_VISIONOS_APP_DIR}/ProfileStore.swift"
    "${MKW_VISIONOS_APP_DIR}/VrHistoryView.swift"
    "${MKW_VISIONOS_APP_DIR}/ProfilesView.swift"
    "${MKW_VISIONOS_APP_DIR}/ZipWriter.swift"
    "${MKW_VISIONOS_APP_DIR}/ProfileTransfer.swift"
    "${MKW_VISIONOS_GENERATED_DIR}/RetroRewindBuild.swift")

add_executable(WiiCompiledVision MACOSX_BUNDLE ${MKW_VISIONOS_APP_SOURCES})
set_target_properties(WiiCompiledVision PROPERTIES
    OUTPUT_NAME "WiiCompiledVision"
    MACOSX_BUNDLE_INFO_PLIST "${MKW_VISIONOS_APP_DIR}/Info.plist"
    XCODE_ATTRIBUTE_GENERATE_INFOPLIST_FILE NO
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${MKW_VISIONOS_APP_BUNDLE_ID}"
    XCODE_ATTRIBUTE_PRODUCT_NAME "WiiCompiledVision"
    XCODE_ATTRIBUTE_MKW_APP_DISPLAY_NAME "${MKW_VISIONOS_APP_NAME}"
    XCODE_ATTRIBUTE_MARKETING_VERSION "1.0"
    XCODE_ATTRIBUTE_CURRENT_PROJECT_VERSION "1"
    # 7 is the Vision device family.
    XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "7"
    XCODE_ATTRIBUTE_SUPPORTED_PLATFORMS "xros xrsimulator"
    XCODE_ATTRIBUTE_XROS_DEPLOYMENT_TARGET "${CMAKE_OSX_DEPLOYMENT_TARGET}"
    XCODE_ATTRIBUTE_SWIFT_VERSION "5.0"
    XCODE_ATTRIBUTE_CLANG_ENABLE_MODULES YES
    XCODE_ATTRIBUTE_CLANG_CXX_LIBRARY "libc++"
    XCODE_ATTRIBUTE_ENABLE_PREVIEWS NO
    XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED YES
    XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED YES
    XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "Apple Development"
    XCODE_ATTRIBUTE_CODE_SIGN_STYLE Automatic
    XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "${MKW_VISIONOS_TEAM}"
    XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS "${MKW_VISIONOS_APP_DIR}/WiiCompiledVision.entitlements"
    XCODE_ATTRIBUTE_DEAD_CODE_STRIPPING YES
    XCODE_ATTRIBUTE_SWIFT_OPTIMIZATION_LEVEL "-O"
    XCODE_ATTRIBUTE_ASSETCATALOG_COMPILER_GENERATE_ASSET_SYMBOLS NO
    # The layered Home View icon in App/Assets.xcassets (a Back/Middle/Front image stack).
    XCODE_ATTRIBUTE_ASSETCATALOG_COMPILER_APPICON_NAME "AppIcon")
# The games are not linked: each is a framework (runtime/cmake/PublicProducts.cmake) embedded in
# the app, and the chosen one is loaded with dlopen when the player presses Play
# (visionos/App/GameLibrary.swift), so the runtime's static initialisers read the Config.toml the
# launcher's settings page wrote. Embedding signs them with the app's identity.
add_dependencies(WiiCompiledVision ${MKW_VISIONOS_GAME_TARGETS})
set_target_properties(WiiCompiledVision PROPERTIES
    XCODE_EMBED_FRAMEWORKS "${MKW_VISIONOS_GAME_TARGETS}"
    XCODE_EMBED_FRAMEWORKS_CODE_SIGN_ON_COPY ON
    XCODE_EMBED_FRAMEWORKS_REMOVE_HEADERS_ON_COPY ON
    XCODE_ATTRIBUTE_LD_RUNPATH_SEARCH_PATHS "@executable_path/Frameworks")

# The runtime's read-only resources, where RuntimePlatform::ExecutableDirectory() looks for them
# (the bundle root is the resource directory on the iOS family): the first-run NAND payload, the
# DSP coefficient ROM and the transferable pipeline cache. The Ninja products copy the same files
# next to the executable (mkw_configure_product), which a framework cannot.
set(MKW_VISIONOS_RUNTIME_ASSETS "${CMAKE_CURRENT_SOURCE_DIR}/../runtime/assets")
# They go through Xcode's own Copy Bundle Resources phase, which runs before the bundle
# is sealed; a post-build copy lands after code signing and invalidates the signature.
# The bootstrap folder is staged under the name the runtime looks for and added as a
# folder reference (a directory source), which Xcode copies whole.
set(MKW_VISIONOS_STAGED_RESOURCES "${CMAKE_CURRENT_BINARY_DIR}/staged-resources")
file(REMOVE_RECURSE "${MKW_VISIONOS_STAGED_RESOURCES}/wii_bootstrap")
file(COPY "${MKW_VISIONOS_RUNTIME_ASSETS}/wii/" DESTINATION "${MKW_VISIONOS_STAGED_RESOURCES}/wii_bootstrap")
set(MKW_VISIONOS_BUNDLED_RESOURCES
    "${MKW_VISIONOS_RUNTIME_ASSETS}/dsp/dsp_coef.bin"
    "${MKW_VISIONOS_RUNTIME_ASSETS}/pipeline/initial_pipeline_cache.db"
    "${MKW_VISIONOS_STAGED_RESOURCES}/wii_bootstrap"
    "${MKW_VISIONOS_APP_DIR}/Assets.xcassets")
target_sources(WiiCompiledVision PRIVATE ${MKW_VISIONOS_BUNDLED_RESOURCES})
set_source_files_properties(${MKW_VISIONOS_BUNDLED_RESOURCES} PROPERTIES
    MACOSX_PACKAGE_LOCATION Resources
    HEADER_FILE_ONLY ON)

if(MKW_VISIONOS_WITH_RETRO_REWIND)
    message(STATUS "visionOS app: ${MKW_VISIONOS_APP_NAME} carrying WiiCompiled and Retro Rewind (pack ${MKW_VISIONOS_RR_VERSION}, Code.pul ${MKW_VISIONOS_RR_CODE_PUL_SHA256})")
else()
    message(STATUS "visionOS app: ${MKW_VISIONOS_APP_NAME} carrying WiiCompiled")
endif()
