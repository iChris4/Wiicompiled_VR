#pragma once

#include "../gfx/common.hpp"
#include "shader_info.hpp"
#include "../gfx/stereo_replay.hpp"
#include <optional>

namespace aurora::gx {
struct DrawData {
  gfx::PipelineRef pipeline;
  // Same GX state with exact fragment-depth export enabled. Bound only when
  // this draw is actually reprojected onto the VR virtual screen.
  gfx::PipelineRef exactScreenDepthPipeline;
  // Eye depth/stencil format siblings. Shader modules are shared with mono.
  gfx::PipelineRef stereoPipeline = 0;
  gfx::PipelineRef stereoScreenPipeline = 0;
  // Constant-alpha (kCompositeSourceBlend) siblings of pipeline and stereoPipeline, set only
  // on the perspective draws of a pipeline an eye re-issues at a composite
  // (note_composite_source_pipeline).
  gfx::PipelineRef compositeSourcePipeline = 0;
  gfx::PipelineRef stereoCompositeSourcePipeline = 0;
  // Multiview siblings of stereoPipeline, stereoScreenPipeline and stereoCompositeSourcePipeline
  // (gfx/stereo_multiview.hpp), set while the device has the feature.
  gfx::PipelineRef multiviewPipeline = 0;
  gfx::PipelineRef multiviewScreenPipeline = 0;
  gfx::PipelineRef multiviewCompositeSourcePipeline = 0;
  gfx::Range vertRange;
  gfx::Range idxRange;
  gfx::Range uniformRange;
  std::array<gfx::Range, MaxInterpolatedFrames> interpolatedUniformRanges;
  std::array<gfx::Range, AURORA_STEREO_EYE_COUNT> stereoUniformRanges;
  gfx::Range previousUniformRange;
  UniformReplayLayout uniformReplayLayout;
  uint32_t vtxCount;
  uint32_t indexCount;
  uint32_t instanceCount;
  bool expandedPrimitive;
  GXBindGroups bindGroups;
  uint32_t dstAlpha;
  // Valid only for simple orthographic rectangles/lines (textured or not).
  // Recorded before merging so VR can omit desktop split masks without
  // modifying GX.
  std::optional<gfx::stereo_replay::SubviewRect> screenRect;
  // The records are in the geometry buffer (gx/geometry_cache.hpp), the indices 32-bit and absolute into it: group 0
  // binds that buffer and the index buffer switches format for the draw.
  bool cachedGeometry = false;
  // The pipeline reads those records through vertex buffer 0 (gx/native_vertex.hpp).
  bool nativeVertices = false;
  // stereoUniformRanges hold two copies of the recorded uniform (gfx prepare_stereo_replay_uniforms): the
  // eyes keep this draw on its recorded transforms, and a multiview pass reads a valid uniform for either
  // view. Such a draw is never folded onto the virtual screen.
  bool stereoPairShared = false;
};

constexpr uint32_t GXPipelineConfigVersion = 21;

// PipelineConfig::stereoStencil: the eye's Depth24PlusStencil8 target, and the constant-alpha
// blend an eye uses to re-issue a composite's source draws (see UniformReplayLayout).
constexpr uint32_t kStereoStencilFormat = 1u;
constexpr uint32_t kCompositeSourceBlend = 2u;

constexpr GXFogType effective_pipeline_fog_type(GXFogType fogType, GXZTexOp zTextureOp, bool zCompLocBeforeTex,
                                                GXBlendMode blendMode, GXLogicOp logicOp) noexcept {
  const bool usesLateZTexture = zTextureOp != GX_ZT_DISABLE && !zCompLocBeforeTex;
  const bool usesUnsupportedLogicFog = blendMode == GX_BM_LOGIC && logicOp != GX_LO_OR;
  return usesLateZTexture && usesUnsupportedLogicFog ? GX_FOG_NONE : fogType;
}

struct PipelineConfig {
  uint32_t version = GXPipelineConfigVersion;
  uint32_t msaaSamples = 1;
  // Bits of kStereoStencilFormat and kCompositeSourceBlend. One field, so the on-disk
  // pipeline recipes recorded before the second bit existed stay valid.
  uint32_t stereoStencil = 0;
  ShaderConfig shaderConfig;
  GXCompare depthFunc;
  GXCullMode cullMode;
  GXBlendMode blendMode;
  GXBlendFactor blendFacSrc, blendFacDst;
  GXLogicOp blendOp;
  GXPixelFmt pixelFmt;
  uint32_t dstAlpha;
  bool depthCompare, depthUpdate, alphaUpdate, colorUpdate;
};
static_assert(std::has_unique_object_representations_v<PipelineConfig>);

inline bool valid_pipeline_config(const PipelineConfig& config) noexcept {
  const auto in_range = [](auto value, auto maximum) {
    using Value = decltype(value);
    return static_cast<std::underlying_type_t<Value>>(value) >= 0 &&
           static_cast<std::underlying_type_t<Value>>(value) <= static_cast<std::underlying_type_t<Value>>(maximum);
  };
  const bool validSamples =
      config.msaaSamples == 1 || config.msaaSamples == 2 || config.msaaSamples == 4 || config.msaaSamples == 8;
  return config.version == GXPipelineConfigVersion && validSamples && config.stereoStencil <= (kStereoStencilFormat | kCompositeSourceBlend) && in_range(config.depthFunc, GX_ALWAYS) &&
         in_range(config.cullMode, GX_CULL_ALL) && in_range(config.blendMode, GX_BM_SUBTRACT) &&
         in_range(config.blendFacSrc, GX_BL_INVDSTALPHA) && in_range(config.blendFacDst, GX_BL_INVDSTALPHA) &&
         in_range(config.blendOp, GX_LO_SET) && in_range(config.pixelFmt, GX_PF_YUV420);
}

wgpu::RenderPipeline create_pipeline([[maybe_unused]] const PipelineConfig& config);
void clear_shader_module_cache();

// Per-pass encoder state carried across the draws of one render pass, so the replay loop can elide Dawn calls that
// would re-bind what is already bound.
struct DrawEncodeState {
  gfx::PipelineRef currentPipeline = UINTPTR_MAX;
  // The bind group currently occupying slot 2.
  WGPUBindGroup boundTextureBindGroup = nullptr;
  // The index buffer binding is established lazily by the first indexed draw and re-bound only when the index format
  // changes (16-bit for the frame's draws, 32-bit for the geometry cache's); every draw addresses it with firstIndex
  // instead of a per-draw SetIndexBuffer.
  wgpu::IndexFormat indexFormat = wgpu::IndexFormat::Undefined;
  // Whether group 0 currently binds the geometry cache's buffer rather than the frame's vertex buffer (the pass
  // starts with the latter bound).
  bool geometryBound = false;
  // Whether vertex buffer 0 (the geometry cache's buffer, for native vertex input) is bound in this pass.
  bool vertexBufferBound = false;
  // A multiview eye pass (gfx/stereo_multiview.hpp): group 1 binds each draw's pair of eye uniforms.
  bool multiview = false;
};

void render(const DrawData& data, const wgpu::RenderPassEncoder& pass, DrawEncodeState& state,
            bool requireReadyPipeline, const gfx::Range* uniformRangeOverride = nullptr,
            gfx::PipelineRef pipelineOverride = 0, const wgpu::Color* blendConstantOverride = nullptr);

// Gives the draws of `pipeline` their constant-alpha siblings from its next draw on: an eye
// re-issues them at a composite (gfx::link_composite_source). Called with the renderer lock
// the draw paths hold, as the composite is recorded.
void note_composite_source_pipeline(gfx::PipelineRef pipeline) noexcept;

void queue_surface(const u8* dlStart, uint32_t dlSize, bool bigEndian) noexcept;
} // namespace aurora::gx
