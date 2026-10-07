#pragma once

#include <cstdint>

#include <webgpu/webgpu_cpp.h>

// Fragment density maps for foveated eye rendering, through the ABI of a Dawn built with Aurora's
// patches (include/aurora/dawn_fdm_abi.h, the Quest build). Against any other Dawn the maps are
// simply unavailable.
namespace aurora::webgpu::fdm {

// Before the device is requested: whether to ask Dawn for VK_EXT_fragment_density_map.
void request(bool wanted) noexcept;
// Once the device exists: reads what Dawn enabled, and logs it.
void device_created() noexcept;
bool available() noexcept;
// Framebuffer pixels per density map texel, in each direction.
uint32_t texel_size() noexcept;

// An immutable RG8 map of `layers` layers (1 to 8) of `width` by `height` texels, packed back to back,
// or 0. Under a multiview pass the view's index picks the layer; one layer serves every view. Usable
// once map_ready.
uint64_t create_map(uint32_t width, uint32_t height, uint32_t layers, const uint8_t* rg8) noexcept;
bool map_ready(uint64_t map) noexcept;
void release_map(uint64_t map) noexcept;
// Render passes whose first color attachment is `view` get `map` while it is ready; 0 unbinds.
bool bind(const wgpu::TextureView& view, uint64_t map) noexcept;

} // namespace aurora::webgpu::fdm
