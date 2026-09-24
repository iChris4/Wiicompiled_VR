include(FetchContent)
include(GoogleTest)

option(AURORA_GPU_SMOKE_TESTS "Build opt-in tests requiring a desktop GPU" OFF)
if (AURORA_GPU_SMOKE_TESTS AND AURORA_ENABLE_GX AND WIN32)
  # Exercises the custom Dawn DLL's Aurora Vulkan ABI (patches/dawn). Run it with that
  # webgpu_dawn.dll beside the executable; the stock prebuilt Dawn lacks the exports and the
  # test reports that at startup. Vulkan headers come from the SDK or any tree on
  # AURORA_TEST_VULKAN_INCLUDE (the runtime's FetchContent copy works).
  find_path(AURORA_TEST_VULKAN_INCLUDE vulkan/vulkan.h HINTS "$ENV{VULKAN_SDK}/Include")
  if (AURORA_TEST_VULKAN_INCLUDE)
    add_executable(vulkan_native_bridge_smoke vulkan_native_bridge_smoke.cpp)
    target_include_directories(vulkan_native_bridge_smoke PRIVATE ../include "${AURORA_TEST_VULKAN_INCLUDE}")
    target_link_libraries(vulkan_native_bridge_smoke PRIVATE dawn::webgpu_dawn dawn::dawncpp_headers)
  else ()
    message(STATUS "vulkan_native_bridge_smoke skipped: no vulkan/vulkan.h (set VULKAN_SDK or AURORA_TEST_VULKAN_INCLUDE)")
  endif ()
  add_executable(stereo_frame_worker_smoke stereo_frame_worker_smoke.cpp)
  target_include_directories(stereo_frame_worker_smoke PRIVATE ../lib)
  target_link_libraries(stereo_frame_worker_smoke PRIVATE aurora::core aurora::gx aurora::main aurora::vi
                        dawn::dawncpp_headers)
  add_executable(stereo_multiplayer_smoke stereo_multiplayer_smoke.cpp)
  target_include_directories(stereo_multiplayer_smoke PRIVATE ../lib)
  target_link_libraries(stereo_multiplayer_smoke PRIVATE aurora::core aurora::gx aurora::main aurora::vi
                        dawn::dawncpp_headers)
  add_executable(efb_ram_lifetime_smoke efb_ram_lifetime_smoke.cpp)
  target_include_directories(efb_ram_lifetime_smoke PRIVATE ../lib)
  target_link_libraries(efb_ram_lifetime_smoke PRIVATE aurora::core aurora::gx aurora::main aurora::vi
                        dawn::dawncpp_headers)
  # VR cockpit overlay (hands, synthetic wheel) against real scene depth. Standalone: it
  # defines the GPU globals itself and needs only the header.
  add_executable(cockpit_gpu_smoke cockpit_gpu_smoke.cpp)
  target_include_directories(cockpit_gpu_smoke PRIVATE ../include ../lib)
  target_compile_definitions(cockpit_gpu_smoke PRIVATE AURORA TARGET_PC WEBGPU_DAWN)
  target_link_libraries(cockpit_gpu_smoke PRIVATE fmt::fmt xxhash absl::flat_hash_map absl::btree
                        dawn::webgpu_dawn dawn::dawncpp_headers TracyClient ${AURORA_SDL3_TARGET})
endif ()

if (NOT TARGET gtest)
  FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.tar.gz
    URL_HASH SHA256=65fab701d9829d38cb77c14acdc431d2108bfdbf8979e40eb8ae567edf10b27c
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    EXCLUDE_FROM_ALL
  )
  FetchContent_MakeAvailable(googletest)
endif ()

if (AURORA_ENABLE_GX)
  # GX FIFO round-trip tests
  # Compile GX sources directly to avoid pulling in the full renderer/WebGPU runtime
  add_executable(gx_fifo_tests
    gx_fifo_test.cpp
    gx_test_stubs.cpp
    stereo_replay_test.cpp
    stereo_interpolation_test.cpp
    stereo_mirror_test.cpp
    eye_pass_plan_test.cpp
    foveation_test.cpp
    native_wheel_test.cpp
    cockpit_geometry_test.cpp
    texture_bind_group_cache_key_test.cpp
    ../lib/gfx/efb_ram_encoder.cpp
    # GX API implementations (encoders)
    ../lib/dolphin/gx/GXBump.cpp
    ../lib/dolphin/gx/GXCull.cpp
    ../lib/dolphin/gx/GXCpu2Efb.cpp
    ../lib/dolphin/gx/GXDispList.cpp
    ../lib/dolphin/gx/GXDraw.cpp
    ../lib/dolphin/gx/GXExtra.cpp
    ../lib/dolphin/gx/GXFifo.cpp
    ../lib/dolphin/gx/GXFrameBuffer.cpp
    ../lib/dolphin/gx/GXGeometry.cpp
    ../lib/dolphin/gx/GXGet.cpp
    ../lib/dolphin/gx/GXLighting.cpp
    ../lib/dolphin/gx/GXManage.cpp
    ../lib/dolphin/gx/GXAurora.cpp
    ../lib/dolphin/gx/GXPixel.cpp
    ../lib/dolphin/gx/GXTev.cpp
    ../lib/dolphin/gx/GXTexture.cpp
    ../lib/dolphin/gx/GXTransform.cpp
    ../lib/dolphin/gx/GXVert.cpp
    # FIFO/command processor (encoder + decoder)
    ../lib/gx/fifo.cpp
    ../lib/gx/command_processor.cpp
    ../lib/gx/shader_info.cpp
    ../lib/gx/frame_interpolation.cpp
  )

  target_include_directories(gx_fifo_tests PRIVATE
    ../include
    ../lib
    ../lib/dolphin/gx
  )

  target_compile_definitions(gx_fifo_tests PRIVATE AURORA TARGET_PC)

  target_link_libraries(gx_fifo_tests PRIVATE
    gtest
    gtest_main
    fmt::fmt
    xxhash
    absl::flat_hash_map
    absl::btree
    dawn::dawncpp_headers
    TracyClient
    ${AURORA_SDL3_TARGET}
  )

  gtest_discover_tests(gx_fifo_tests)

  # The Dawn blob cache's background writer, against a real SQLite database in a temp folder.
  if (TARGET sqlite3)
    add_executable(gpu_cache_tests gpu_cache_test.cpp ../lib/webgpu/gpu_cache.cpp)
    target_include_directories(gpu_cache_tests PRIVATE ../include ../lib)
    target_compile_definitions(gpu_cache_tests PRIVATE AURORA TARGET_PC)
    target_link_libraries(gpu_cache_tests PRIVATE gtest gtest_main fmt::fmt xxhash sqlite3 dawn::dawncpp_headers
                          TracyClient ${AURORA_SDL3_TARGET})
    if (AURORA_CACHE_USE_ZSTD)
      target_compile_definitions(gpu_cache_tests PRIVATE AURORA_CACHE_USE_ZSTD)
      target_link_libraries(gpu_cache_tests PRIVATE libzstd_static)
    endif ()
    gtest_discover_tests(gpu_cache_tests)
  endif ()
endif () # AURORA_ENABLE_GX

# DVD API tests
if (TARGET aurora::dvd)
  add_executable(dvd_tests test_dvd.cpp)
  target_link_libraries(dvd_tests PRIVATE aurora::dvd gtest gtest_main)

  # Pass disc image path as a compile definition if provided
  if (DEFINED DVD_TEST_IMAGE AND EXISTS "${DVD_TEST_IMAGE}")
    target_compile_definitions(dvd_tests PRIVATE DVD_TEST_IMAGE="${DVD_TEST_IMAGE}")
  endif ()

  gtest_discover_tests(dvd_tests)
endif ()

add_executable(os_alloc_tests
  os_alloc_test.cpp
  os_test_globals.cpp
  ../lib/dolphin/os/OSAlloc.cpp
  ../lib/logging.cpp
)
target_include_directories(os_alloc_tests PRIVATE
  ../include
)
target_compile_definitions(os_alloc_tests PRIVATE AURORA TARGET_PC)
target_link_libraries(os_alloc_tests PRIVATE gtest gtest_main fmt::fmt)
gtest_discover_tests(os_alloc_tests)
