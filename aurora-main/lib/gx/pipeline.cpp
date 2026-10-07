#include "pipeline.hpp"

#include "../gfx/geometry_buffer.hpp"
#include "../webgpu/gpu.hpp"
#include "native_vertex.hpp"
#include "gx_fmt.hpp"
#include "shader_info.hpp"
#include "tracy/Tracy.hpp"

#include <condition_variable>
#include <memory>
#include <mutex>

#include <absl/container/flat_hash_map.h>

namespace aurora::gx {
static Module Log("aurora::gx");

namespace {
struct ShaderConfigHash {
  size_t operator()(const ShaderConfig& config) const noexcept { return static_cast<size_t>(xxh3_hash(config)); }
};

struct CachedShaderModule {
  std::condition_variable ready;
  wgpu::ShaderModule module;
  bool compiling = true;
};

std::mutex sShaderModuleCacheMutex;
absl::flat_hash_map<ShaderConfig, std::shared_ptr<CachedShaderModule>, ShaderConfigHash> sShaderModuleCache;

wgpu::ShaderModule cached_shader_module(const ShaderConfig& config) {
  std::shared_ptr<CachedShaderModule> entry;
  {
    std::unique_lock lock{sShaderModuleCacheMutex};
    const auto it = sShaderModuleCache.find(config);
    if (it == sShaderModuleCache.end()) {
      entry = std::make_shared<CachedShaderModule>();
      sShaderModuleCache.emplace(config, entry);
    } else {
      entry = it->second;
      entry->ready.wait(lock, [&] { return !entry->compiling; });
      return entry->module;
    }
  }

  auto module = build_shader(config);
  {
    std::lock_guard lock{sShaderModuleCacheMutex};
    entry->module = module;
    entry->compiling = false;
  }
  entry->ready.notify_all();
  return module;
}
} // namespace

wgpu::RenderPipeline create_pipeline(const PipelineConfig& config) {
  ZoneScoped;
  const auto shader = cached_shader_module(config.shaderConfig);
  if (config.shaderConfig.nativeVertices) {
    // The geometry cache's records through the vertex-input stage (gx/native_vertex.hpp).
    const auto native = native_vertex::layout(config.shaderConfig);
    CHECK(native.valid, "Unsupported native vertex layout reached pipeline creation");
    wgpu::VertexBufferLayout buffer{};
    buffer.arrayStride = config.shaderConfig.vtxStride;
    buffer.stepMode = wgpu::VertexStepMode::Vertex;
    buffer.attributeCount = native.count;
    buffer.attributes = native.attributes.data();
    const std::array buffers{buffer};
    return build_pipeline(config, buffers, shader, "GX Pipeline");
  }
  return build_pipeline(config, {}, shader, "GX Pipeline");
}

void clear_shader_module_cache() {
  std::lock_guard lock{sShaderModuleCacheMutex};
  sShaderModuleCache.clear();
}

void render(const DrawData& data, const wgpu::RenderPassEncoder& pass, DrawEncodeState& state,
            bool requireReadyPipeline, const gfx::Range* uniformRangeOverride, gfx::PipelineRef pipelineOverride,
            const wgpu::Color* blendConstantOverride) {
  const gfx::PipelineRef pipeline = pipelineOverride != 0 ? pipelineOverride : data.pipeline;
  if (!gfx::bind_pipeline(pipeline, pass, state.currentPipeline, requireReadyPipeline)) {
    return;
  }

  // An interpolated presentation slot re-encodes the identical draw with only this range replaced; overriding here
  // avoids copying the whole DrawData per draw per slot.
  const gfx::Range& uniformRange = uniformRangeOverride != nullptr ? *uniformRangeOverride : data.uniformRange;
  const std::array offsets{uniformRange.offset};
  pass.SetBindGroup(1, state.multiview ? gfx::g_multiviewUniformBindGroup : gfx::g_uniformBindGroup,
                    offsets.size(), offsets.data());
  // Resolved when the draw was recorded; see GXBindGroups.
  if (data.bindGroups.resolvedTextureBindGroup != nullptr &&
      data.bindGroups.resolvedTextureBindGroup != state.boundTextureBindGroup) {
    wgpuRenderPassEncoderSetBindGroup(pass.Get(), 2, data.bindGroups.resolvedTextureBindGroup, 0, nullptr);
    state.boundTextureBindGroup = data.bindGroups.resolvedTextureBindGroup;
  }
  if (blendConstantOverride != nullptr) {
    pass.SetBlendConstant(blendConstantOverride);
  } else if (data.dstAlpha != UINT32_MAX) {
    const wgpu::Color color{0.f, 0.f, 0.f, data.dstAlpha / 255.f};
    pass.SetBlendConstant(&color);
  }
  // A geometry-cache draw reads its records from the persistent buffer through group 0 and its indices as 32-bit
  // values; both bindings switch only when consecutive draws differ in that.
  if (data.cachedGeometry != state.geometryBound) {
    pass.SetBindGroup(0, data.cachedGeometry ? gfx::detail::geometry_bind_group() : gfx::g_staticBindGroup);
    state.geometryBound = data.cachedGeometry;
  }
  if (data.nativeVertices && !state.vertexBufferBound) {
    // Bound once for the pass; a pipeline without vertex buffers leaves the slot alone.
    pass.SetVertexBuffer(0, gfx::detail::geometry_buffer());
    state.vertexBufferBound = true;
  }
  const wgpu::IndexFormat indexFormat = data.cachedGeometry ? wgpu::IndexFormat::Uint32 : wgpu::IndexFormat::Uint16;
  if (state.indexFormat != indexFormat) {
    // Bound for the pass; draws select their range with firstIndex below.
    pass.SetIndexBuffer(gfx::g_indexBuffer, indexFormat, 0, wgpu::kWholeSize);
    state.indexFormat = indexFormat;
  }
  const uint32_t indexBytes = data.cachedGeometry ? sizeof(uint32_t) : sizeof(uint16_t);
  pass.DrawIndexed(data.indexCount, data.instanceCount, static_cast<uint32_t>(data.idxRange.offset / indexBytes));
}
} // namespace aurora::gx
