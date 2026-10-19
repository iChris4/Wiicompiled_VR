# Public WiiCompiled product graph.
#
# The translator owns the translated build graph. Mario Kart's profile-neutral
# functions are compiled once into mkw_base_shared; only callers whose direct
# ABI differs between profiles receive small base/RR variants.

# Per-game data the runtime reads but does not own. Kept out of the runtime
# library so that library stays game-independent (and can be precompiled).
set(MKW_GAME_DATA_SOURCES "")
# The translator's output tree. It sits next to runtime/ in the installer's
# build workspace; a build configured straight from a checkout (the Android
# app, the test build dirs) names the workspace copy instead.
set(MKW_GENERATED_DIR "${MKW_RUNTIME_SOURCE_DIR}/../generated" CACHE PATH
    "Translator output directory holding data_sections_init.cpp and RuntimeConfig.h")
get_filename_component(MKW_GENERATED_PARENT_DIR "${MKW_GENERATED_DIR}" DIRECTORY)

set(DATA_INIT_FILE "${MKW_GENERATED_DIR}/data_sections_init.cpp")
set(DATA_INIT_BLOB_ASM "${MKW_GENERATED_DIR}/data_sections_init_blobs.S")
if(MKW_PLATFORM_ANDROID AND EXISTS "${DATA_INIT_BLOB_ASM}")
    # The Windows installer generates the blob assembly for PE/COFF. The payload
    # is byte-identical on every platform (the .S only wraps .incbin
    # directives), so rewrite the section syntax for ELF here rather than
    # requiring a second translation pass for the Quest build. A tree generated
    # with `generate-data-init --target-os android` passes through unchanged.
    file(READ "${DATA_INIT_BLOB_ASM}" MKW_ANDROID_BLOB_ASM_TEXT)
    string(REPLACE ".section .rdata,\"dr\"" ".section .rodata,\"a\",@progbits"
        MKW_ANDROID_BLOB_ASM_ELF "${MKW_ANDROID_BLOB_ASM_TEXT}")
    if(NOT MKW_ANDROID_BLOB_ASM_ELF STREQUAL MKW_ANDROID_BLOB_ASM_TEXT)
        string(APPEND MKW_ANDROID_BLOB_ASM_ELF "\n.section .note.GNU-stack,\"\",@progbits\n")
        set(DATA_INIT_BLOB_ASM "${CMAKE_CURRENT_BINARY_DIR}/data_sections_init_blobs_android.S")
        file(WRITE "${DATA_INIT_BLOB_ASM}" "${MKW_ANDROID_BLOB_ASM_ELF}")
        message(STATUS "Rewrote the PE/COFF blob assembly for ELF: ${DATA_INIT_BLOB_ASM}")
    endif()
endif()
if(EXISTS "${DATA_INIT_FILE}")
    list(APPEND MKW_GAME_DATA_SOURCES "${DATA_INIT_FILE}")
endif()
# Crash-report symbolization table emitted by generate-data-init. The stub
# (deliberately outside the globbed src/ tree so it is never picked up twice)
# keeps link succeeding when the generated table has not been produced yet.
set(GUEST_SYMBOL_TABLE_FILE "${MKW_GENERATED_DIR}/guest_symbol_table.cpp")
if(EXISTS "${GUEST_SYMBOL_TABLE_FILE}")
    list(APPEND MKW_GAME_DATA_SOURCES "${GUEST_SYMBOL_TABLE_FILE}")
else()
    list(APPEND MKW_GAME_DATA_SOURCES "${MKW_RUNTIME_SOURCE_DIR}/cmake/guest_symbol_table_stub.cpp")
endif()
if(EXISTS "${DATA_INIT_BLOB_ASM}")
    enable_language(ASM)
    set_source_files_properties("${DATA_INIT_BLOB_ASM}" PROPERTIES LANGUAGE ASM SKIP_UNITY_BUILD_INCLUSION ON)
    list(APPEND MKW_GAME_DATA_SOURCES "${DATA_INIT_BLOB_ASM}")
endif()
# Runtime glue that depends on generated/ headers (e.g. RuntimeConfig.h).
file(GLOB MKW_GAME_GLUE_SOURCES CONFIGURE_DEPENDS "${MKW_RUNTIME_SOURCE_DIR}/game/*.cpp")
list(APPEND MKW_GAME_DATA_SOURCES ${MKW_GAME_GLUE_SOURCES})

if(MKW_PLATFORM_MACOS)
    find_library(MKW_IOKIT_FRAMEWORK IOKit REQUIRED)
    find_library(MKW_COREFOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
endif()

function(mkw_apply_translated_compile_options target)
    # The NDK toolchain adds -fstack-protector-strong to every object. Translated guest code keeps
    # its state in guest memory and the CpuContext, so the canaries only cost cycles there.
    target_compile_options(${target} PRIVATE
        -O2 ${MKW_TRANSLATED_PPC_FP_OPTIONS} -fno-slp-vectorize -w -pipe
        $<$<PLATFORM_ID:Android>:-fno-stack-protector>)
endfunction()

# Translated shard TUs are the memory-hungry compiles; everything else in the build is
# comparatively small. A dedicated Ninja job pool caps how many of them run at once so the
# global parallelism can use every core for the cheap TUs without the memory ceiling being a
# guess. The pool depth is per-machine (derived from installed RAM by LocalBuild.ps1) and is
# deliberately not part of the canonical flag set: it changes scheduling, never output bytes.
if(MKW_TRANSLATED_COMPILE_JOBS GREATER 0)
    set_property(GLOBAL APPEND PROPERTY JOB_POOLS "mkw_translated=${MKW_TRANSLATED_COMPILE_JOBS}")
endif()

function(mkw_bound_translated_compiles target)
    if(MKW_TRANSLATED_COMPILE_JOBS GREATER 0)
        set_property(TARGET ${target} PROPERTY JOB_POOL_COMPILE mkw_translated)
    endif()
endfunction()

function(mkw_configure_translated_target target)
    mkw_configure_object_target(${target})
    mkw_apply_translated_compile_options(${target})
    mkw_bound_translated_compiles(${target})
endfunction()

mkw_select_runtime_provider()
if(MKW_RUNTIME_USE_PREBUILT)
    mkw_import_prebuilt_runtime(mkw_runtime_prebuilt "${MKW_NP_RUNTIME_ARCHIVE_PATH}")
    # A replacement archive can carry an older mtime than the last link, so relink on fingerprint change.
    set(MKW_RUNTIME_PREBUILT_STAMP "${CMAKE_BINARY_DIR}/mkw_runtime_prebuilt.stamp")
    file(CONFIGURE OUTPUT "${MKW_RUNTIME_PREBUILT_STAMP}" CONTENT "${MKW_NP_RUNTIME_FINGERPRINT}\n")
else()
    mkw_add_runtime_library(mkw_runtime_common OBJECT ${SOURCES})
endif()

add_library(mkw_game_data OBJECT ${MKW_GAME_DATA_SOURCES})
mkw_configure_object_target(mkw_game_data)
target_compile_features(mkw_game_data PRIVATE cxx_std_20)
# On Android SDL owns the entry point: main.cpp includes SDL_main.h so that
# SDLActivity's nativeRunMain finds SDL_main inside libmain.so.
target_compile_definitions(mkw_game_data PRIVATE
    $<$<NOT:$<PLATFORM_ID:Android>>:SDL_MAIN_HANDLED>
    _DISABLE_STRING_ANNOTATION _DISABLE_VECTOR_ANNOTATION)
if(MKW_SETUP_VERSION)
    set_property(SOURCE "${MKW_RUNTIME_SOURCE_DIR}/game/setup_version.cpp" APPEND PROPERTY
        COMPILE_DEFINITIONS "MKW_SETUP_VERSION=\"${MKW_SETUP_VERSION}\"")
endif()
mkw_runtime_link_dependencies(mkw_game_data PRIVATE)
set_target_properties(mkw_game_data PROPERTIES UNITY_BUILD OFF)
mkw_apply_common_compile_options(mkw_game_data)

# Host ISA guard deliberately keeps the plain baseline ISA and checks the
# selected x86 profile before optional instructions can execute. AArch64 has
# no equivalent optional ISA floor to probe: NEON/FMA are architectural
# requirements.
if(MKW_HOST_IS_X86_64)
    add_library(mkw_cpu_baseline OBJECT "${MKW_CPU_BASELINE_SOURCE}")
    target_compile_features(mkw_cpu_baseline PRIVATE cxx_std_17)
    set_target_properties(mkw_cpu_baseline PROPERTIES UNITY_BUILD OFF)
    target_compile_options(mkw_cpu_baseline PRIVATE -w)
    if(MKW_X86_CPU_PROFILE STREQUAL "v2")
        target_compile_definitions(mkw_cpu_baseline PRIVATE MKW_X86_CPU_PROFILE_V2=1)
    endif()
endif()

if(NOT MKW_BASE_COMMON_SHARDS)
    message(FATAL_ERROR "Translator build graph contains no shared base shards")
endif()

# Translated code compiles against the guest ABI surface only, so runtime
# implementation edits do not invalidate its PCH.
set(MKW_TRANSLATED_PCH "${MKW_RUNTIME_SOURCE_DIR}/include/guest_abi/guest_abi_pch.h")
if(NOT EXISTS "${MKW_TRANSLATED_PCH}")
    set(MKW_TRANSLATED_PCH "${MKW_RUNTIME_SOURCE_DIR}/include/mkw_pch.h")
endif()

add_library(mkw_base_shared STATIC ${MKW_BASE_COMMON_SHARDS})
mkw_configure_translated_target(mkw_base_shared)
target_precompile_headers(mkw_base_shared PRIVATE "${MKW_TRANSLATED_PCH}")

# Registration and indirect-dispatch tables are translator output too; they
# compile with the translated configuration rather than inside the products.
add_library(mkw_base_registration OBJECT ${MKW_BASE_REGISTRATION_SOURCES})
mkw_configure_translated_target(mkw_base_registration)
target_precompile_headers(mkw_base_registration REUSE_FROM mkw_base_shared)

if(MKW_BASE_PORTABLE_SENSITIVE_SHARDS)
    add_library(mkw_base_sensitive OBJECT ${MKW_BASE_PORTABLE_SENSITIVE_SHARDS})
    mkw_configure_translated_target(mkw_base_sensitive)
    target_precompile_headers(mkw_base_sensitive REUSE_FROM mkw_base_shared)
endif()

if(MKW_HAVE_RETRO_REWIND)
    if(MKW_RETRO_PORTABLE_SENSITIVE_SHARDS)
        add_library(mkw_retro_sensitive OBJECT ${MKW_RETRO_PORTABLE_SENSITIVE_SHARDS})
        mkw_configure_translated_target(mkw_retro_sensitive)
        target_precompile_headers(mkw_retro_sensitive REUSE_FROM mkw_base_shared)
    endif()

    if(MKW_PLATFORM_ANDROID)
        # Same PE/COFF-to-ELF rewrite as the base data blobs above.
        set(MKW_ANDROID_RETRO_EXTRA_SOURCES)
        foreach(source IN LISTS MKW_RETRO_EXTRA_SOURCES)
            if(source MATCHES "\\.S$")
                file(READ "${source}" MKW_ANDROID_RETRO_BLOB_TEXT)
                string(REPLACE ".section .rdata,\"dr\"" ".section .rodata,\"a\",@progbits"
                    MKW_ANDROID_RETRO_BLOB_ELF "${MKW_ANDROID_RETRO_BLOB_TEXT}")
                if(NOT MKW_ANDROID_RETRO_BLOB_ELF STREQUAL MKW_ANDROID_RETRO_BLOB_TEXT)
                    string(APPEND MKW_ANDROID_RETRO_BLOB_ELF "\n.section .note.GNU-stack,\"\",@progbits\n")
                    get_filename_component(source_name "${source}" NAME_WE)
                    set(source "${CMAKE_CURRENT_BINARY_DIR}/${source_name}_android.S")
                    file(WRITE "${source}" "${MKW_ANDROID_RETRO_BLOB_ELF}")
                endif()
            endif()
            list(APPEND MKW_ANDROID_RETRO_EXTRA_SOURCES "${source}")
        endforeach()
        set(MKW_RETRO_EXTRA_SOURCES ${MKW_ANDROID_RETRO_EXTRA_SOURCES})
    endif()

    set(MKW_RETRO_TRANSLATED_SOURCES ${MKW_RETRO_MOD_SHARDS} ${MKW_RETRO_EXTRA_SOURCES})
    set(MKW_RETRO_BLOB_OBJECTS)
    foreach(source IN LISTS MKW_RETRO_EXTRA_SOURCES)
        if(source MATCHES "\\.S$")
            enable_language(ASM)
            set_source_files_properties("${source}" PROPERTIES LANGUAGE ASM SKIP_PRECOMPILE_HEADERS ON)
        endif()
    endforeach()
    # mod_data_patches (MKW_RETRO_EXTRA_SOURCES) calls runtime APIs directly; it
    # keeps the full runtime include path this target already has.
    add_library(mkw_retro_rewind_functions OBJECT ${MKW_RETRO_TRANSLATED_SOURCES})
    mkw_configure_translated_target(mkw_retro_rewind_functions)
    target_precompile_headers(mkw_retro_rewind_functions REUSE_FROM mkw_base_shared)

    add_library(mkw_retro_registration OBJECT ${MKW_RETRO_REGISTRATION_SOURCES})
    mkw_configure_translated_target(mkw_retro_registration)
    target_precompile_headers(mkw_retro_registration REUSE_FROM mkw_base_shared)
endif()

# WITHOUT_GAME configures everything a product links except the translated game: the base
# shards and the game data generated from the disc. The Quest game kit is built that way.
function(mkw_configure_product target)
    cmake_parse_arguments(PARSE_ARGV 1 MKW_PRODUCT "WITHOUT_GAME" "" "")
    if(TARGET mkw_runtime_common)
        target_sources(${target} PRIVATE $<TARGET_OBJECTS:mkw_runtime_common>)
    else()
        # Natives self-register from static constructors, so every archive
        # member must be kept. Listed before mkw_base_shared so single-pass
        # linkers resolve the runtime's references to translated functions.
        target_link_libraries(${target} PRIVATE "$<LINK_LIBRARY:WHOLE_ARCHIVE,mkw_runtime_prebuilt>")
        set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS "${MKW_RUNTIME_PREBUILT_STAMP}")
    endif()
    if(MKW_PRODUCT_WITHOUT_GAME)
        target_sources(${target} PRIVATE
            "$<FILTER:$<TARGET_OBJECTS:mkw_game_data>,EXCLUDE,data_sections_init|guest_symbol_table>")
    else()
        target_sources(${target} PRIVATE $<TARGET_OBJECTS:mkw_game_data>)
    endif()
    # Startup CPU check. Must stay a separate object library so it keeps the
    # plain baseline ISA while everything around it is built for x86-64-v3.
    if(MKW_HOST_IS_X86_64)
        target_sources(${target} PRIVATE $<TARGET_OBJECTS:mkw_cpu_baseline>)
    endif()
    target_include_directories(${target} PRIVATE
        "${MKW_RUNTIME_SOURCE_DIR}/include"
        "${MKW_RUNTIME_SOURCE_DIR}/src"
        # Workspace root, so translator output is spelled "generated/<x>.h"
        # instead of a ../ chain whose depth depends on the includer.
        "${MKW_GENERATED_PARENT_DIR}"
        "${MKW_RUNTIME_SOURCE_DIR}/.."
        "${MKW_RUNTIME_SOURCE_DIR}/../aurora-main/include")
    target_compile_definitions(${target} PRIVATE
        $<$<NOT:$<PLATFORM_ID:Android>>:SDL_MAIN_HANDLED>
        _DISABLE_STRING_ANNOTATION _DISABLE_VECTOR_ANNOTATION TARGET_PC)
    target_compile_features(${target} PRIVATE cxx_std_20)
    mkw_apply_common_compile_options(${target})
    if(MKW_PRODUCT_WITHOUT_GAME)
        target_link_libraries(${target} PRIVATE
            mkw_platform mkw::pugixml mkw::toml11 mkw::cryptopp mkw::mbedtls)
    else()
        target_link_libraries(${target} PRIVATE
            mkw_platform mkw_base_shared mkw::pugixml mkw::toml11 mkw::cryptopp mkw::mbedtls)
    endif()

    target_link_libraries(${target} PRIVATE
        aurora::gx aurora::pad aurora::si aurora::vi aurora::mtx)
    if(MKW_ENABLE_OPENXR)
        # mkw_runtime_common is consumed as raw object files, so its private
        # loader dependency must also be present on each final product link.
        target_link_libraries(${target} PRIVATE ${MKW_OPENXR_TARGET})
    endif()
    if(MKW_PLATFORM_MACOS)
        target_link_libraries(${target} PRIVATE
            "${MKW_IOKIT_FRAMEWORK}" "${MKW_COREFOUNDATION_FRAMEWORK}" "${MKW_COREAUDIO_FRAMEWORK}")
        target_link_options(${target} PRIVATE
            "LINKER:-U,_OBJC_CLASS_$_MTLLogStateDescriptor")
    endif()
    if(EXISTS "${MKW_AURORA_DIR}/cmake/AuroraCopyRuntimeDLLs.cmake")
        include("${MKW_AURORA_DIR}/cmake/AuroraCopyRuntimeDLLs.cmake")
        aurora_copy_runtime_dlls(${target})
    endif()
    if(TARGET sqlite3)
        get_target_property(MKW_SQLITE_TARGET_TYPE sqlite3 TYPE)
    endif()
    if(TARGET sqlite3 AND
       (MKW_SQLITE_TARGET_TYPE STREQUAL "SHARED_LIBRARY" OR
        MKW_SQLITE_TARGET_TYPE STREQUAL "MODULE_LIBRARY"))
        add_custom_command(TARGET ${target} POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:sqlite3> $<TARGET_FILE_DIR:${target}> VERBATIM)
    endif()

    if(MKW_PLATFORM_WINDOWS)
        target_link_libraries(${target} PRIVATE
            dbghelp user32 winmm ws2_32 iphlpapi secur32 crypt32 windowsapp)

        set_target_properties(${target} PROPERTIES WIN32_EXECUTABLE TRUE)
    elseif(MKW_PLATFORM_LINUX OR MKW_PLATFORM_MACOS_X86_64)
        # mkw_runtime_common is an OBJECT library: WiiCompiled/RetroRewind only pull in its .o
        # files via $<TARGET_OBJECTS:>, which does not propagate mkw_runtime_common's own
        # target_link_libraries (object libraries don't carry usage requirements to a consumer
        # that isn't itself linked against as a target). fiber_manager.cpp's co_* calls live in
        # those objects, so the actual executable link needs mkw::libco directly, same as it
        # needs it independently of the platform branch above. ${CMAKE_DL_LIBS} is
        # here for the same reason: music_attenuation.cpp's dlopen(libdbus-1) lives in those
        # objects (empty string on glibc >= 2.34, where dl* is in libc).
        target_link_libraries(${target} PRIVATE mkw::libco ${CMAKE_DL_LIBS})
    elseif(MKW_PLATFORM_ANDROID)
        target_link_libraries(${target} PRIVATE mkw::libco android log vulkan ${CMAKE_DL_LIBS})
        # A shared library's own calls and data references go through the PLT and GOT unless the
        # symbols are bound at link time; with 29,000 translated functions calling each other and
        # the runtime, those stubs were 4% of the game thread on the Quest. Nothing interposes
        # symbols of the game library, and the JNI and SDL_main exports stay exported.
        target_link_options(${target} PRIVATE "-Wl,-Bsymbolic")
    endif()
    if(MKW_PLATFORM_WINDOWS)
        foreach(runtime_dll libc++.dll libunwind.dll)
            execute_process(
                COMMAND "${CMAKE_CXX_COMPILER}" "--print-file-name=${runtime_dll}"
                OUTPUT_VARIABLE runtime_dll_path
                OUTPUT_STRIP_TRAILING_WHITESPACE)
            if(NOT EXISTS "${runtime_dll_path}")
                get_filename_component(mkw_compiler_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
                set(runtime_dll_path "${mkw_compiler_bin}/${runtime_dll}")
            endif()
            if(NOT EXISTS "${runtime_dll_path}")
                message(FATAL_ERROR "llvm-mingw runtime DLL not found: ${runtime_dll}")
            endif()
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${runtime_dll_path}" $<TARGET_FILE_DIR:${target}> VERBATIM)
        endforeach()
    endif()

    set(MKW_WII_BOOTSTRAP_SOURCE_DIR "${MKW_RUNTIME_SOURCE_DIR}/assets/wii")
    if(NOT EXISTS "${MKW_WII_BOOTSTRAP_SOURCE_DIR}/shared2/wc24")
        message(FATAL_ERROR "Missing Wii first-run bootstrap payload: ${MKW_WII_BOOTSTRAP_SOURCE_DIR}")
    endif()
    add_custom_command(TARGET ${target} POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${MKW_WII_BOOTSTRAP_SOURCE_DIR}" "$<TARGET_FILE_DIR:${target}>/wii_bootstrap" VERBATIM)

    set(MKW_DSP_COEFFICIENT_ROM "${MKW_RUNTIME_SOURCE_DIR}/assets/dsp/dsp_coef.bin")
    if(NOT EXISTS "${MKW_DSP_COEFFICIENT_ROM}")
        message(FATAL_ERROR "Missing Wii DSP coefficient ROM: ${MKW_DSP_COEFFICIENT_ROM}")
    endif()
    file(SHA256 "${MKW_DSP_COEFFICIENT_ROM}" MKW_DSP_COEFFICIENT_ROM_SHA256)
    if(NOT MKW_DSP_COEFFICIENT_ROM_SHA256 STREQUAL
       "d7741279c2e8ec5c5fb318f8fbdd6de6bf583520d288e836a5383233a4238179")
        message(FATAL_ERROR "Wii DSP coefficient ROM hash mismatch: ${MKW_DSP_COEFFICIENT_ROM_SHA256}")
    endif()
    add_custom_command(TARGET ${target} POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${MKW_DSP_COEFFICIENT_ROM}" "$<TARGET_FILE_DIR:${target}>/dsp_coef.bin" VERBATIM)

    # Aurora imports this portable recipe database into each user's writable
    # pipeline cache. Keep the upstream filename so its default resourcesPath
    # lookup works without application-specific configuration.
    set(MKW_INITIAL_PIPELINE_CACHE
        "${MKW_RUNTIME_SOURCE_DIR}/assets/pipeline/initial_pipeline_cache.db")
    if(NOT EXISTS "${MKW_INITIAL_PIPELINE_CACHE}")
        message(FATAL_ERROR "Missing transferable Aurora pipeline cache: ${MKW_INITIAL_PIPELINE_CACHE}")
    endif()
    add_custom_command(TARGET ${target} POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${MKW_INITIAL_PIPELINE_CACHE}"
        "$<TARGET_FILE_DIR:${target}>/initial_pipeline_cache.db" VERBATIM)

    # Non-Windows TLS (runtime/src/hle/net/network_ssl.cpp's mbed TLS path) needs a trusted root
    # CA bundle to verify server certificates against - Windows gets this for free from the OS via
    # Schannel, mbed TLS does not ship one itself. Not SHA256-pinned like the DSP ROM above: unlike
    # a fixed hardware ROM, this bundle is expected to be refreshed periodically as CAs rotate.
    # Windows gets its trust store from Schannel, so only the platforms that actually build the
    # mbed TLS path need the bundle beside the executable.
    if(NOT MKW_PLATFORM_WINDOWS)
        set(MKW_CA_CERTIFICATE_BUNDLE "${MKW_RUNTIME_SOURCE_DIR}/assets/certs/cacert.pem")
        if(NOT EXISTS "${MKW_CA_CERTIFICATE_BUNDLE}")
            message(FATAL_ERROR "Missing TLS root CA bundle: ${MKW_CA_CERTIFICATE_BUNDLE}")
        endif()
        add_custom_command(TARGET ${target} POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${MKW_CA_CERTIFICATE_BUNDLE}" "$<TARGET_FILE_DIR:${target}>/cacert.pem" VERBATIM)
    endif()
endfunction()

# Android ships each product as the shared library SDLActivity loads; the base
# game keeps SDL's conventional "main" name and Retro Rewind gets its own so a
# dual-flavour app can bundle both without a name clash.
if(MKW_PLATFORM_ANDROID)
    add_library(WiiCompiled SHARED "${MKW_BASE_PRODUCT_SOURCE}")
    set_target_properties(WiiCompiled PROPERTIES OUTPUT_NAME main)
else()
    add_executable(WiiCompiled "${MKW_BASE_PRODUCT_SOURCE}")
endif()
mkw_configure_product(WiiCompiled)
target_sources(WiiCompiled PRIVATE $<TARGET_OBJECTS:mkw_base_registration>)
target_precompile_headers(WiiCompiled PRIVATE
    "${MKW_RUNTIME_SOURCE_DIR}/include/mkw_pch.h")
if(TARGET mkw_base_sensitive)
    target_sources(WiiCompiled PRIVATE $<TARGET_OBJECTS:mkw_base_sensitive>)
endif()

if(MKW_PLATFORM_ANDROID)
    # The Quest game kit. The APK never contains the translated game: the player builds
    # libmain.so from their own disc, on the headset or on a PC, and the app loads it from
    # private storage. This probe is WiiCompiled without the game, linked with the game's
    # symbols left unresolved. Building it builds every input the kit ships, and its link line
    # is the recipe android/QuestGameKit.psm1 exports and the game builders replay.
    add_library(mkw_quest_kit_probe SHARED "${MKW_BASE_PRODUCT_SOURCE}")
    mkw_configure_product(mkw_quest_kit_probe WITHOUT_GAME)
    target_precompile_headers(mkw_quest_kit_probe REUSE_FROM WiiCompiled)
    target_link_options(mkw_quest_kit_probe PRIVATE "-Wl,--unresolved-symbols=ignore-all")
endif()

if(MKW_HAVE_RETRO_REWIND)
    if(MKW_PLATFORM_ANDROID)
        add_library(RetroRewind SHARED "${MKW_RETRO_REWIND_PRODUCT_SOURCE}")
        set_target_properties(RetroRewind PROPERTIES OUTPUT_NAME main_retro_rewind)
    else()
        add_executable(RetroRewind "${MKW_RETRO_REWIND_PRODUCT_SOURCE}")
    endif()
    mkw_configure_product(RetroRewind)
    target_precompile_headers(RetroRewind REUSE_FROM WiiCompiled)
    target_sources(RetroRewind PRIVATE $<TARGET_OBJECTS:mkw_retro_registration>)
    if(TARGET mkw_retro_sensitive)
        target_sources(RetroRewind PRIVATE $<TARGET_OBJECTS:mkw_retro_sensitive>)
    endif()
    target_sources(RetroRewind PRIVATE $<TARGET_OBJECTS:mkw_retro_rewind_functions>)
    if(MKW_RETRO_BLOB_OBJECTS)
        target_sources(RetroRewind PRIVATE ${MKW_RETRO_BLOB_OBJECTS})
    endif()
    if(MKW_PLATFORM_ANDROID)
        # Retro Rewind's game kit, the same idea as the base probe above: RetroRewind without any
        # translated code, so the APK carries neither the game nor the mod.
        add_library(mkw_quest_kit_probe_retro SHARED "${MKW_RETRO_REWIND_PRODUCT_SOURCE}")
        mkw_configure_product(mkw_quest_kit_probe_retro WITHOUT_GAME)
        target_precompile_headers(mkw_quest_kit_probe_retro REUSE_FROM WiiCompiled)
        target_link_options(mkw_quest_kit_probe_retro PRIVATE "-Wl,--unresolved-symbols=ignore-all")
    endif()
    add_custom_target(mkw_release DEPENDS WiiCompiled RetroRewind)
else()
    add_custom_target(mkw_release DEPENDS WiiCompiled)
    message(STATUS "RetroRewind target disabled (run translate-mod and emit-build-shards)")
endif()

# mkw_runtime_common gets the same flags from mkw_add_runtime_library.
set(MKW_ALL_BUILD_TARGETS
    mkw_game_data mkw_base_shared mkw_base_registration mkw_base_sensitive mkw_retro_sensitive
    mkw_retro_rewind_functions mkw_retro_registration WiiCompiled RetroRewind
    mkw_quest_kit_probe mkw_quest_kit_probe_retro)
foreach(target IN LISTS MKW_ALL_BUILD_TARGETS)
    if(TARGET ${target})
        mkw_apply_baseline_arch(${target})
    endif()
endforeach()
