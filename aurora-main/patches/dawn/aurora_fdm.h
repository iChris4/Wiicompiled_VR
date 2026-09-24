// Included only by the pinned Dawn source build (apply.py): fragment density maps for Aurora's
// immersive eye render passes. Implemented in aurora_fdm.inc.
#pragma once
#include "src/dawn/common/vulkan_platform.h"

namespace dawn::native {
class TextureViewBase;
}

namespace dawn::native::vulkan {
class Device;
struct VulkanDeviceInfo;

// Device::CreateDevice keeps VK_EXT_fragment_density_map only when Aurora asked for it and the
// device can attach a map to a dynamic rendering pass over ordinary (non-subsampled) images.
bool AuroraFdmWanted(const VulkanDeviceInfo& info, bool dynamicRendering);
// The density map for a render pass whose first color attachment is `view`, or a null handle.
::VkImageView AuroraFdmViewFor(Device* device, TextureViewBase* view);
// Device::DestroyImpl: releases every map while the fenced deleter still runs.
void AuroraFdmDestroy(Device* device);
}  // namespace dawn::native::vulkan
