// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>
// Versioned C ABI for the fragment density maps the patched Dawn attaches to Aurora's immersive
// eye render passes (VK_EXT_fragment_density_map through dynamic rendering, see
// patches/dawn/aurora_fdm.inc). The Quest build links that Dawn statically and
// AuroraDawnProvider.cmake defines AURORA_DAWN_FDM_ABI when the package carries it. Devices and
// texture views are WGPUDevice and WGPUTextureView handles.
#define AURORA_DAWN_FDM_ABI 1

#if defined(AURORA_DAWN_FDM_IMPLEMENTATION)
#define AURORA_DAWN_FDM_API DAWN_NATIVE_EXPORT
#else
#define AURORA_DAWN_FDM_API
#endif

#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
  // VK_EXT_fragment_density_map is enabled on the device, for non-subsampled attachments.
  uint32_t enabled;
  // The framebuffer area one density map texel may cover.
  uint32_t minTexelWidth;
  uint32_t minTexelHeight;
  uint32_t maxTexelWidth;
  uint32_t maxTexelHeight;
} AuroraDawnFdmCaps;

AURORA_DAWN_FDM_API uint32_t AuroraDawnFdmVersion(void);
// Asks the next Vulkan device Dawn creates for fragment density maps. They are only enabled when
// that device renders through dynamic rendering and supports non-subsampled attachments. Call it
// before requesting the device.
AURORA_DAWN_FDM_API void AuroraDawnFdmRequest(int enable);
// Returns caps->enabled.
AURORA_DAWN_FDM_API int AuroraDawnFdmQuery(void* device, AuroraDawnFdmCaps* caps);
// Uploads an immutable RG8 density map, `width` by `height` texels in packed rows, and returns its
// id, or 0 on failure. A map becomes usable once its upload has completed on the GPU: the driver
// reads a non-dynamic map on the CPU when a render pass using it is recorded.
AURORA_DAWN_FDM_API uint64_t AuroraDawnFdmCreateMap(void* device, uint32_t width, uint32_t height,
                                                    const uint8_t* rg8);
AURORA_DAWN_FDM_API int AuroraDawnFdmMapReady(void* device, uint64_t map);
// Frees the map once the GPU is done with it, and unbinds it from any view.
AURORA_DAWN_FDM_API void AuroraDawnFdmReleaseMap(void* device, uint64_t map);
// Every render pass whose first color attachment is `view` gets `map` as its fragment density map
// while the map is ready; 0 unbinds. The binding keeps the view alive until it is unbound. Returns 0
// when the map is unknown or too small to cover the view.
AURORA_DAWN_FDM_API int AuroraDawnFdmBind(void* device, void* view, uint64_t map);
#ifdef __cplusplus
}
#endif
