#pragma once
// Draws an eye into an image of Dawn's device that the OpenXR runtime will show: a shared
// AHardwareBuffer's texture (vulkan_interop.cpp) or an acquired swapchain image
// (vulkan_direct_interop.cpp). One triangle covering the target, each pixel loaded from the eye's
// texture. On the Quest's Adreno a transfer copy is the slow path: the Metroid Prime port, on this
// renderer, measured a copy of both eyes into the runtime's images at about 2.6 ms a frame against
// 0.4 ms for the draw, and here the draw took 0.2 to 0.6 ms of App GPU time off a Luigi Circuit race
// at render_scale 0.8. An eye smaller than its image (the immersive window's eyes) lands in the
// top-left corner as the copy did; the rest is cleared. Frame worker only.

#include "../stereo.hpp"
#include "gpu.hpp"

#include <array>
#include <cstdint>
#include <utility>
#include <vector>

namespace aurora::webgpu {

// The eyes, then the settings panel's layer image after them.
inline constexpr uint32_t kEyeBlitSlots = AURORA_STEREO_EYE_COUNT + 1;

class EyeBlitter final {
public:
  // width and height: the part of the target the eye fills, which is the eye's size, or less than
  // it while a resolution change leaves an eye larger than its image.
  bool Encode(wgpu::CommandEncoder& encoder, uint32_t slot, const stereo::EyeImage& source,
              const wgpu::TextureView& target, wgpu::TextureFormat targetFormat, uint32_t width,
              uint32_t height) noexcept {
    if (source.view == nullptr || !*source.view || !target || slot >= kEyeBlitSlots || width == 0 || height == 0 ||
        !Initialize()) {
      return false;
    }
    const wgpu::RenderPipeline& pipeline = PipelineFor(targetFormat);
    if (!pipeline) {
      return false;
    }
    // One bind group per slot, remade when the eye's view changes (a render-scale change). The group
    // keeps its view alive, so a cached handle cannot be reused by another view meanwhile.
    auto& cached = m_bindGroups[slot];
    if (cached.first != source.view->Get() || !cached.second) {
      const wgpu::BindGroupEntry entry{.binding = 0, .textureView = *source.view};
      const wgpu::BindGroupDescriptor bindGroupDescriptor{
          .label = "OpenXR eye blit",
          .layout = m_layout,
          .entryCount = 1,
          .entries = &entry,
      };
      cached = {source.view->Get(), g_device.CreateBindGroup(&bindGroupDescriptor)};
      if (!cached.second) {
        return false;
      }
    }
    const wgpu::RenderPassColorAttachment attachment{
        .view = target,
        .loadOp = wgpu::LoadOp::Clear,
        .storeOp = wgpu::StoreOp::Store,
        .clearValue = {0.0, 0.0, 0.0, 0.0},
    };
    const wgpu::RenderPassDescriptor passDescriptor{
        .label = "OpenXR eye blit",
        .colorAttachmentCount = 1,
        .colorAttachments = &attachment,
    };
    const auto pass = encoder.BeginRenderPass(&passDescriptor);
    pass.SetViewport(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f);
    pass.SetScissorRect(0, 0, width, height);
    pass.SetPipeline(pipeline);
    pass.SetBindGroup(0, cached.second);
    pass.Draw(3);
    pass.End();
    return true;
  }

  // Drops the cached bind groups (and with them the eye views they hold).
  void Reset() noexcept {
    for (auto& group : m_bindGroups) {
      group = {};
    }
  }

private:
  bool Initialize() noexcept {
    if (m_layout) {
      return true;
    }
    static constexpr char kShader[] = R"(
@group(0) @binding(0) var eye: texture_2d<f32>;

@vertex
fn vs_main(@builtin(vertex_index) index: u32) -> @builtin(position) vec4f {
    let uv = vec2f(f32((index << 1u) & 2u), f32(index & 2u));
    return vec4f(uv * vec2f(2.0, -2.0) + vec2f(-1.0, 1.0), 0.0, 1.0);
}

@fragment
fn fs_raw(@builtin(position) position: vec4f) -> @location(0) vec4f {
    return textureLoad(eye, vec2i(position.xy), 0);
}

// The eyes hold gamma-encoded bytes and an sRGB attachment encodes what it is given: decode first.
@fragment
fn fs_srgb(@builtin(position) position: vec4f) -> @location(0) vec4f {
    let color = textureLoad(eye, vec2i(position.xy), 0);
    let low = color.rgb / 12.92;
    let high = pow((color.rgb + 0.055) / 1.055, vec3f(2.4));
    return vec4f(select(high, low, color.rgb <= vec3f(0.04045)), color.a);
}
)";
    wgpu::ShaderSourceWGSL wgsl{};
    wgsl.code = kShader;
    const wgpu::ShaderModuleDescriptor moduleDescriptor{.nextInChain = &wgsl, .label = "OpenXR eye blit"};
    m_module = g_device.CreateShaderModule(&moduleDescriptor);
    const wgpu::BindGroupLayoutEntry entry{
        .binding = 0,
        .visibility = wgpu::ShaderStage::Fragment,
        .texture =
            {
                .sampleType = wgpu::TextureSampleType::UnfilterableFloat,
                .viewDimension = wgpu::TextureViewDimension::e2D,
            },
    };
    const wgpu::BindGroupLayoutDescriptor layoutDescriptor{
        .label = "OpenXR eye blit",
        .entryCount = 1,
        .entries = &entry,
    };
    m_layout = g_device.CreateBindGroupLayout(&layoutDescriptor);
    const wgpu::PipelineLayoutDescriptor pipelineLayoutDescriptor{
        .label = "OpenXR eye blit",
        .bindGroupLayoutCount = 1,
        .bindGroupLayouts = &m_layout,
    };
    m_pipelineLayout = g_device.CreatePipelineLayout(&pipelineLayoutDescriptor);
    return m_module && m_layout && m_pipelineLayout;
  }

  const wgpu::RenderPipeline& PipelineFor(wgpu::TextureFormat format) noexcept {
    for (const auto& [cached, pipeline] : m_pipelines) {
      if (cached == format) {
        return pipeline;
      }
    }
    const bool srgb =
        format == wgpu::TextureFormat::RGBA8UnormSrgb || format == wgpu::TextureFormat::BGRA8UnormSrgb;
    const wgpu::ColorTargetState target{.format = format};
    const wgpu::FragmentState fragment{
        .module = m_module,
        .entryPoint = srgb ? "fs_srgb" : "fs_raw",
        .targetCount = 1,
        .targets = &target,
    };
    const wgpu::RenderPipelineDescriptor descriptor{
        .label = "OpenXR eye blit",
        .layout = m_pipelineLayout,
        .vertex = {.module = m_module, .entryPoint = "vs_main"},
        .primitive = {.topology = wgpu::PrimitiveTopology::TriangleList},
        .multisample = {.count = 1},
        .fragment = &fragment,
    };
    m_pipelines.emplace_back(format, g_device.CreateRenderPipeline(&descriptor));
    return m_pipelines.back().second;
  }

  wgpu::ShaderModule m_module;
  wgpu::BindGroupLayout m_layout;
  wgpu::PipelineLayout m_pipelineLayout;
  std::vector<std::pair<wgpu::TextureFormat, wgpu::RenderPipeline>> m_pipelines;
  std::array<std::pair<WGPUTextureView, wgpu::BindGroup>, kEyeBlitSlots> m_bindGroups{};
};

} // namespace aurora::webgpu
