#pragma once

#include "common.hpp"

#include <webgpu/webgpu_cpp.h>

namespace aurora::gfx::clear {
struct DrawData {
  PipelineRef pipeline;
  PipelineRef stereoPipeline = 0;
  // Its multiview sibling (gfx/stereo_multiview.hpp), while the device has the feature.
  PipelineRef multiviewPipeline = 0;
  Range uniformRange;
  wgpu::Color color;
  float depth = 0.f;
  bool useScissor = false;
  // Set for the EFB clear a GX copy performs after copying. Immersive replay
  // can drop these: an eye attachment is not the Wii's reused EFB, so the
  // post-copy reset would erase the image the copy just published.
  bool copyClear = false;
  ClipRect scissor{};
};

constexpr uint32_t ClearPipelineConfigVersion = 5;
struct PipelineConfig {
  uint32_t version = ClearPipelineConfigVersion;
  uint32_t msaaSamples = 1;
  bool clearColor = true;
  bool clearAlpha = true;
  bool clearDepth = true;
  bool stereoStencil = false;
  // Both layers of a multiview eye pass (gfx/stereo_multiview.hpp); needs stereoStencil.
  bool multiview = false;
  uint8_t pad[3] = {};
};
static_assert(std::has_unique_object_representations_v<PipelineConfig>);

wgpu::RenderPipeline create_pipeline(const PipelineConfig& config);
void render(const DrawData& data, const wgpu::RenderPassEncoder& pass, const wgpu::Extent3D& targetSize,
            PipelineRef& currentPipeline);
} // namespace aurora::gfx::clear
