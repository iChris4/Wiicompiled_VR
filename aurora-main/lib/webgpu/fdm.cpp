#include "fdm.hpp"

#include "../internal.hpp"
#include "gpu.hpp"

#include <algorithm>
#include <bit>

// AURORA_DAWN_FDM carries the ABI version of the Dawn package's patches (AuroraDawnProvider.cmake).
#if defined(__ANDROID__) && defined(AURORA_DAWN_FDM)
#include <aurora/dawn_fdm_abi.h>
static_assert(AURORA_DAWN_FDM == AURORA_DAWN_FDM_ABI,
              "The Dawn package's fragment density map ABI does not match include/aurora/dawn_fdm_abi.h");
#define AURORA_FDM_SUPPORTED 1
#endif

namespace aurora::webgpu::fdm {
namespace {
Module Log("aurora::webgpu::fdm");

bool g_requested = false;
bool g_available = false;
uint32_t g_texelSize = 0;

// Meta's own maps use 32: rings smooth enough, in a map of a few hundred texels.
constexpr uint32_t kPreferredTexelSize = 32;
} // namespace

void request(bool wanted) noexcept {
  g_requested = wanted;
#ifdef AURORA_FDM_SUPPORTED
  AuroraDawnFdmRequest(wanted ? 1 : 0);
#endif
}

void device_created() noexcept {
  g_available = false;
  g_texelSize = 0;
#ifdef AURORA_FDM_SUPPORTED
  AuroraDawnFdmCaps caps{};
  if (AuroraDawnFdmQuery(g_device.Get(), &caps) != 0) {
    // Density texels cover a power-of-two area within the device's range in each direction.
    const uint32_t smallest = std::max({caps.minTexelWidth, caps.minTexelHeight, 1u});
    const uint32_t largest = std::max(std::min(caps.maxTexelWidth, caps.maxTexelHeight), smallest);
    g_texelSize = std::bit_ceil(std::clamp(kPreferredTexelSize, smallest, largest));
    g_available = true;
    Log.info("Fragment density maps: enabled, {}x{} to {}x{} pixels per texel, using {}", caps.minTexelWidth,
             caps.minTexelHeight, caps.maxTexelWidth, caps.maxTexelHeight, g_texelSize);
  } else if (g_requested) {
    Log.warn("Fragment density maps: unavailable; the device lacks VK_EXT_fragment_density_map for "
             "non-subsampled images, or Dawn does not render through dynamic rendering");
  }
#else
  if (g_requested) {
    Log.warn("Fragment density maps: unavailable; this build links a Dawn without Aurora's patches "
             "(android/Build-QuestDawn.ps1)");
  }
#endif
}

bool available() noexcept { return g_available; }

uint32_t texel_size() noexcept { return g_texelSize; }

uint64_t create_map(uint32_t width, uint32_t height, const uint8_t* rg8) noexcept {
#ifdef AURORA_FDM_SUPPORTED
  if (g_available) {
    return AuroraDawnFdmCreateMap(g_device.Get(), width, height, rg8);
  }
#endif
  (void)width;
  (void)height;
  (void)rg8;
  return 0;
}

bool map_ready(uint64_t map) noexcept {
#ifdef AURORA_FDM_SUPPORTED
  if (g_available && map != 0) {
    return AuroraDawnFdmMapReady(g_device.Get(), map) != 0;
  }
#endif
  (void)map;
  return false;
}

void release_map(uint64_t map) noexcept {
#ifdef AURORA_FDM_SUPPORTED
  if (g_available && map != 0) {
    AuroraDawnFdmReleaseMap(g_device.Get(), map);
  }
#endif
  (void)map;
}

bool bind(const wgpu::TextureView& view, uint64_t map) noexcept {
#ifdef AURORA_FDM_SUPPORTED
  if (g_available && view) {
    return AuroraDawnFdmBind(g_device.Get(), view.Get(), map) != 0;
  }
#endif
  (void)view;
  (void)map;
  return false;
}

} // namespace aurora::webgpu::fdm
