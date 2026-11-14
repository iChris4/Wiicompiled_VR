#include "stereo_overlay.hpp"

#include "gfx/common.hpp"
#include "gfx/stereo_replay.hpp"
#include "imgui.hpp"
#include "webgpu/gpu.hpp"

#include <aurora/aurora.h>
#include <imgui.h>

#include "tracy/Tracy.hpp"

#include <array>
#include <atomic>
#include <cmath>

namespace aurora::stereo_overlay {
namespace {

using webgpu::g_device;
using webgpu::g_queue;

// Uploaded as-is into a WGSL mat4x4<f32>, like the GX uniforms' matrices.
static_assert(sizeof(Mat4x4<float>) == 64);

// The panel's corners, (-1, 1) top left to (1, -1) bottom right, carried through
// one clip-from-panel matrix per eye. UVs are interpolated perspective-correct,
// so the texture stays straight on a panel seen at an angle. The texture holds
// premultiplied colour, which is what ImGui's blending leaves in a cleared target.
constexpr const char* kShader = R"""(
struct Panel {
    clip_from_panel: mat4x4<f32>,
};
@group(0) @binding(0)
var<uniform> panel: Panel;
@group(0) @binding(1)
var panel_sampler: sampler;
@group(0) @binding(2)
var panel_texture: texture_2d<f32>;

struct VertexOutput {
    @builtin(position) pos: vec4<f32>,
    @location(0) uv: vec2<f32>,
};

var<private> corners: array<vec2<f32>, 4> = array<vec2<f32>, 4>(
    vec2(-1.0, 1.0),
    vec2(-1.0, -1.0),
    vec2(1.0, 1.0),
    vec2(1.0, -1.0),
);

@vertex
fn vs_main(@builtin(vertex_index) vtxIdx: u32) -> VertexOutput {
    let corner = corners[vtxIdx];
    var out: VertexOutput;
    // Row-vector convention, like the GX shaders: m0..m3 are the clip x/y/z/w rows.
    out.pos = vec4<f32>(corner, 0.0, 1.0) * panel.clip_from_panel;
    out.uv = vec2<f32>(0.5 + 0.5 * corner.x, 0.5 - 0.5 * corner.y);
    return out;
}

@fragment
fn fs_main(in: VertexOutput) -> @location(0) vec4<f32> {
    return textureSample(panel_texture, panel_sampler, in.uv);
}
)""";

// One ImGui canvas laid over the eyes: the settings panel or the badge.
struct Slot {
  webgpu::TextureWithSampler texture;
  std::array<wgpu::Buffer, AURORA_STEREO_EYE_COUNT> uniforms;
  std::array<wgpu::BindGroup, AURORA_STEREO_EYE_COUNT> bindGroups;
  float widthFraction = 0.f;
  float centerX = 0.f;
  float centerY = 0.f;
  bool visible = false;
};

struct State {
  Slot panelSlot;
  Slot badgeSlot;
  wgpu::RenderPipeline pipeline;
  wgpu::BindGroupLayout bindGroupLayout;
  wgpu::TextureFormat pipelineFormat = wgpu::TextureFormat::Undefined;
  // Stands in for the panel in a layer while it is not showing.
  webgpu::TextureWithSampler transparent;
  bool transparentCleared = false;
};
State g_state;
std::atomic_bool g_layerMode{false};

bool ensure_pipeline() {
  auto& state = g_state;
  const auto format = webgpu::g_graphicsConfig.surfaceConfiguration.format;
  if (state.pipeline && state.pipelineFormat == format) {
    return true;
  }
  state.pipeline = {};
  state.panelSlot.bindGroups = {};
  state.badgeSlot.bindGroups = {};

  wgpu::ShaderSourceWGSL source{};
  source.code = kShader;
  const wgpu::ShaderModuleDescriptor moduleDescriptor{
      .nextInChain = &source,
      .label = "Headset panel module",
  };
  const auto module = g_device.CreateShaderModule(&moduleDescriptor);

  const std::array layoutEntries{
      wgpu::BindGroupLayoutEntry{
          .binding = 0,
          .visibility = wgpu::ShaderStage::Vertex,
          .buffer =
              wgpu::BufferBindingLayout{
                  .type = wgpu::BufferBindingType::Uniform,
                  .minBindingSize = sizeof(Mat4x4<float>),
              },
      },
      wgpu::BindGroupLayoutEntry{
          .binding = 1,
          .visibility = wgpu::ShaderStage::Fragment,
          .sampler =
              wgpu::SamplerBindingLayout{
                  .type = wgpu::SamplerBindingType::Filtering,
              },
      },
      wgpu::BindGroupLayoutEntry{
          .binding = 2,
          .visibility = wgpu::ShaderStage::Fragment,
          .texture =
              wgpu::TextureBindingLayout{
                  .sampleType = wgpu::TextureSampleType::Float,
                  .viewDimension = wgpu::TextureViewDimension::e2D,
              },
      },
  };
  const wgpu::BindGroupLayoutDescriptor layoutDescriptor{
      .label = "Headset panel bind group layout",
      .entryCount = layoutEntries.size(),
      .entries = layoutEntries.data(),
  };
  state.bindGroupLayout = g_device.CreateBindGroupLayout(&layoutDescriptor);
  const wgpu::PipelineLayoutDescriptor pipelineLayoutDescriptor{
      .label = "Headset panel pipeline layout",
      .bindGroupLayoutCount = 1,
      .bindGroupLayouts = &state.bindGroupLayout,
  };
  const auto pipelineLayout = g_device.CreatePipelineLayout(&pipelineLayoutDescriptor);

  constexpr wgpu::BlendComponent kPremultipliedOver{
      .operation = wgpu::BlendOperation::Add,
      .srcFactor = wgpu::BlendFactor::One,
      .dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha,
  };
  const wgpu::BlendState blend{
      .color = kPremultipliedOver,
      .alpha = kPremultipliedOver,
  };
  const std::array colorTargets{wgpu::ColorTargetState{
      .format = format,
      .blend = &blend,
      .writeMask = wgpu::ColorWriteMask::All,
  }};
  const wgpu::FragmentState fragmentState{
      .module = module,
      .entryPoint = "fs_main",
      .targetCount = colorTargets.size(),
      .targets = colorTargets.data(),
  };
  const wgpu::RenderPipelineDescriptor pipelineDescriptor{
      .label = "Headset panel pipeline",
      .layout = pipelineLayout,
      .vertex =
          wgpu::VertexState{
              .module = module,
              .entryPoint = "vs_main",
          },
      .primitive =
          wgpu::PrimitiveState{
              .topology = wgpu::PrimitiveTopology::TriangleStrip,
              .cullMode = wgpu::CullMode::None,
          },
      .multisample =
          wgpu::MultisampleState{
              .count = 1,
              .mask = UINT32_MAX,
          },
      .fragment = &fragmentState,
  };
  state.pipeline = g_device.CreateRenderPipeline(&pipelineDescriptor);
  if (!state.pipeline) {
    return false;
  }
  for (Slot* slot : {&state.panelSlot, &state.badgeSlot}) {
    for (auto& uniform : slot->uniforms) {
      if (!uniform) {
        const wgpu::BufferDescriptor bufferDescriptor{
            .label = "Headset panel uniform",
            .usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst,
            .size = sizeof(Mat4x4<float>),
        };
        uniform = g_device.CreateBuffer(&bufferDescriptor);
      }
    }
  }
  state.pipelineFormat = format;
  return true;
}

void composite(Slot& slot, const wgpu::CommandEncoder& encoder, const wgpu::TextureView& target,
               const Mat4x4<float>& clipFromPanel, uint32_t eyeIndex) noexcept {
  auto& state = g_state;
  if (!slot.visible || !state.pipeline || eyeIndex >= AURORA_STEREO_EYE_COUNT || !target) {
    return;
  }
  auto& bindGroup = slot.bindGroups[eyeIndex];
  if (!bindGroup) {
    const std::array entries{
        wgpu::BindGroupEntry{
            .binding = 0,
            .buffer = slot.uniforms[eyeIndex],
            .size = sizeof(Mat4x4<float>),
        },
        wgpu::BindGroupEntry{
            .binding = 1,
            .sampler = slot.texture.sampler,
        },
        wgpu::BindGroupEntry{
            .binding = 2,
            .textureView = slot.texture.view,
        },
    };
    const wgpu::BindGroupDescriptor descriptor{
        .label = "Headset panel bind group",
        .layout = state.bindGroupLayout,
        .entryCount = entries.size(),
        .entries = entries.data(),
    };
    bindGroup = g_device.CreateBindGroup(&descriptor);
  }
  // Each eye has its own uniform, and every pass that reads it is submitted
  // before this worker writes it again.
  g_queue.WriteBuffer(slot.uniforms[eyeIndex], 0, &clipFromPanel, sizeof(clipFromPanel));

  const std::array attachments{
      wgpu::RenderPassColorAttachment{
          .view = target,
          .loadOp = wgpu::LoadOp::Load,
          .storeOp = wgpu::StoreOp::Store,
      },
  };
  const wgpu::RenderPassDescriptor descriptor{
      .label = eyeIndex == 0 ? "Headset panel left eye" : "Headset panel right eye",
      .colorAttachmentCount = attachments.size(),
      .colorAttachments = attachments.data(),
      .timestampWrites = gfx::gpu_timing_pass(gfx::GpuTimingCategory::Panel),
  };
  const auto pass = encoder.BeginRenderPass(&descriptor);
  pass.SetPipeline(state.pipeline);
  pass.SetBindGroup(0, bindGroup, 0, nullptr);
  pass.Draw(4);
  pass.End();
}

float slot_aspect(const Slot& slot) noexcept {
  const auto& size = slot.texture.size;
  return size.height != 0 ? static_cast<float>(size.width) / static_cast<float>(size.height) : 0.f;
}

wgpu::CommandBuffer prepare_slot(Slot& slot, ImDrawData* drawData, float widthFraction, const char* label) noexcept {
  ZoneScoped;
  auto& state = g_state;
  slot.visible = false;
  if (drawData == nullptr || !(widthFraction > 0.f)) {
    return {};
  }
  const auto width = static_cast<uint32_t>(std::lround(drawData->DisplaySize.x * drawData->FramebufferScale.x));
  const auto height = static_cast<uint32_t>(std::lround(drawData->DisplaySize.y * drawData->FramebufferScale.y));
  if (width == 0 || height == 0 || !ensure_pipeline()) {
    return {};
  }
  const auto format = webgpu::g_graphicsConfig.surfaceConfiguration.format;
  if (!slot.texture.texture || slot.texture.size.width != width || slot.texture.size.height != height ||
      slot.texture.format != format) {
    slot.texture = webgpu::create_render_texture(width, height, false);
    slot.bindGroups = {};
  }
  // The ImGui backend sets its viewport from the draw data, so a texture the
  // device clamped to a smaller size cannot hold the pass.
  if (slot.texture.size.width != width || slot.texture.size.height != height) {
    return {};
  }

  const wgpu::CommandEncoderDescriptor encoderDescriptor{
      .label = label,
  };
  auto encoder = g_device.CreateCommandEncoder(&encoderDescriptor);
  const std::array attachments{
      wgpu::RenderPassColorAttachment{
          .view = slot.texture.view,
          .loadOp = wgpu::LoadOp::Clear,
          .storeOp = wgpu::StoreOp::Store,
          .clearValue = {.r = 0.0, .g = 0.0, .b = 0.0, .a = 0.0},
      },
  };
  const wgpu::RenderPassDescriptor passDescriptor{
      .label = label,
      .colorAttachmentCount = attachments.size(),
      .colorAttachments = attachments.data(),
      .timestampWrites = gfx::gpu_timing_pass(gfx::GpuTimingCategory::Panel),
  };
  bool drawn = false;
  {
    const auto pass = encoder.BeginRenderPass(&passDescriptor);
    drawn = imgui::render_draw_data(pass, drawData);
    pass.End();
  }
  if (!drawn) {
    return {};
  }
  slot.widthFraction = widthFraction;
  slot.visible = true;
  return encoder.Finish();
}

} // namespace

wgpu::CommandBuffer prepare(ImDrawData* drawData, float widthFraction) noexcept {
  return prepare_slot(g_state.panelSlot, drawData, widthFraction, "Headset panel ImGui pass");
}

wgpu::CommandBuffer prepare_badge(ImDrawData* drawData, float widthFraction, float centerX, float centerY) noexcept {
  auto buffer = prepare_slot(g_state.badgeSlot, drawData, widthFraction, "Headset badge ImGui pass");
  g_state.badgeSlot.centerX = centerX;
  g_state.badgeSlot.centerY = centerY;
  return buffer;
}

void composite_immersive(const wgpu::CommandEncoder& encoder, const wgpu::TextureView& eye,
                         const Mat4x4<float>& eyeFrustum, const Mat3x4<float>& viewFromCenter,
                         uint32_t eyeIndex) noexcept {
  auto& state = g_state;
  if (state.panelSlot.visible && !layer_mode()) {
    float screenWidth = 0.f;
    float screenDistance = 0.f;
    gfx::get_stereo_hud_screen_size(screenWidth, screenDistance);
    const auto panel = gfx::stereo_replay::overlay_panel_on_screen(screenWidth, screenDistance,
                                                                   state.panelSlot.widthFraction,
                                                                   slot_aspect(state.panelSlot));
    if (panel.valid()) {
      composite(state.panelSlot, encoder, eye,
                gfx::stereo_replay::compose_overlay_panel_projection(eyeFrustum, viewFromCenter, panel), eyeIndex);
    }
  }
  if (state.badgeSlot.visible) {
    gfx::stereo_replay::HudScreen screen;
    gfx::get_stereo_window_screen(screen.halfWidth, screen.halfHeight, screen.distance);
    const auto badge = gfx::stereo_replay::overlay_panel_at(screen, state.badgeSlot.widthFraction,
                                                            slot_aspect(state.badgeSlot), state.badgeSlot.centerX,
                                                            state.badgeSlot.centerY);
    if (badge.valid()) {
      composite(state.badgeSlot, encoder, eye,
                gfx::stereo_replay::compose_overlay_panel_projection(eyeFrustum, viewFromCenter, badge), eyeIndex);
    }
  }
}

void composite_flat(const wgpu::CommandEncoder& encoder, const wgpu::TextureView& eye, const wgpu::Extent3D& size,
                    uint32_t eyeIndex) noexcept {
  auto& slot = g_state.panelSlot;
  if (!slot.visible || layer_mode() || size.width == 0 || size.height == 0) {
    return;
  }
  const float imageAspect = static_cast<float>(size.width) / static_cast<float>(size.height);
  composite(slot, encoder, eye,
            gfx::stereo_replay::overlay_panel_flat_projection(slot.widthFraction, slot_aspect(slot), imageAspect),
            eyeIndex);
}

void set_layer_mode(bool enabled) noexcept { g_layerMode.store(enabled, std::memory_order_release); }

bool layer_mode() noexcept { return g_layerMode.load(std::memory_order_acquire); }

bool layer_source(const wgpu::CommandEncoder& encoder, uint32_t width, uint32_t height, stereo::EyeImage& out) noexcept {
  auto& state = g_state;
  const auto format = webgpu::g_graphicsConfig.surfaceConfiguration.format;
  if (width == 0 || height == 0) {
    return false;
  }
  const auto& panel = state.panelSlot;
  if (panel.visible && panel.texture.texture && panel.texture.size.width == width &&
      panel.texture.size.height == height && panel.texture.format == format) {
    out = {.texture = &panel.texture.texture, .view = &panel.texture.view, .size = panel.texture.size, .format = format};
    return true;
  }
  if (!state.transparent.texture || state.transparent.size.width != width ||
      state.transparent.size.height != height || state.transparent.format != format) {
    state.transparent = webgpu::create_render_texture(width, height, false);
    state.transparentCleared = false;
    if (state.transparent.size.width != width || state.transparent.size.height != height) {
      state.transparent = {};
      return false;
    }
  }
  if (!state.transparentCleared) {
    const std::array attachments{
        wgpu::RenderPassColorAttachment{
            .view = state.transparent.view,
            .loadOp = wgpu::LoadOp::Clear,
            .storeOp = wgpu::StoreOp::Store,
            .clearValue = {.r = 0.0, .g = 0.0, .b = 0.0, .a = 0.0},
        },
    };
    const wgpu::RenderPassDescriptor descriptor{
        .label = "Headset panel layer clear",
        .colorAttachmentCount = attachments.size(),
        .colorAttachments = attachments.data(),
    };
    encoder.BeginRenderPass(&descriptor).End();
    state.transparentCleared = true;
  }
  out = {.texture = &state.transparent.texture, .view = &state.transparent.view, .size = state.transparent.size,
         .format = format};
  return true;
}

void shutdown() noexcept { g_state = {}; }

} // namespace aurora::stereo_overlay
