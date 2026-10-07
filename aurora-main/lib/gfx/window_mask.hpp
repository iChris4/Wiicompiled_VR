// SPDX-License-Identifier: GPL-3.0-or-later
//
// The immersive window (AuroraStereoFrame::window): after an eye's last draw,
// one full-screen triangle keeps what the eye sees through the 2D layer's
// screen and makes the rest transparent black, so the compositor shows its own
// background (the room, with passthrough) around the race. See OPENXR.md, "The
// immersive window".
#pragma once

#include "common.hpp"
#include "stereo_multiview.hpp"
#include "stereo_replay.hpp"
#include "../webgpu/gpu.hpp"

#include <array>
#include <cstddef>
#include <utility>

namespace aurora::gfx::window_mask {

struct Vertex {
  float position[2];
  // stereo_replay::WindowMask's (u, v, w) at this corner.
  float window[3];
};

struct PipelineKey {
  wgpu::TextureFormat format = wgpu::TextureFormat::Undefined;
  // Undefined: a pass with no depth attachment.
  wgpu::TextureFormat depthFormat = wgpu::TextureFormat::Undefined;
  uint32_t samples = 0;

  bool operator==(const PipelineKey&) const = default;
};

// The eye passes (with depth) use one attachment layout and the mono fallback (without) another;
// each keeps its own pipeline, rebuilt when its format or sample count changes. A third draws both
// eyes at once in a multiview pass (gfx/stereo_multiview.hpp), each eye's triangle from its own
// vertex buffer slot.
inline std::array<std::pair<PipelineKey, wgpu::RenderPipeline>, 3> pipelines;
inline std::array<wgpu::Buffer, AURORA_STEREO_EYE_COUNT> vertexBuffers;

inline void shutdown() {
  pipelines = {};
  vertexBuffers = {};
}

// The multiview pipeline for the eye passes' attachment layout.
inline const wgpu::RenderPipeline& pipeline_multiview(const PipelineKey& key) {
  auto& slot = pipelines[2];
  if (slot.second && slot.first == key) {
    return slot.second;
  }
  slot = {};
#if defined(WGPU_DAWN_RENDER_PIPELINE_MULTIVIEW_INIT)
  wgpu::ShaderSourceWGSL source{};
  source.code = R"(
    enable chromium_experimental_multiview;
    struct Out { @builtin(position) position: vec4f, @location(0) window: vec3f };
    @vertex fn vs(@builtin(view_index) view: u32, @location(0) position0: vec2f, @location(1) window0: vec3f,
                  @location(2) position1: vec2f, @location(3) window1: vec3f) -> Out {
      var o: Out;
      let right = view == 1u;
      o.position = vec4f(select(position0, position1, right), 0.5, 1.0);
      o.window = select(window0, window1, right);
      return o;
    }
    @fragment fn fs(i: Out) -> @location(0) vec4f {
      let p = i.window.xy / i.window.z;
      let edge = 1.0 - abs(p);
      let ramp = clamp(edge / max(fwidth(edge), vec2f(1e-6)) + 0.5, vec2f(0.0), vec2f(1.0));
      let seen = i.window.z > 0.0 && all(abs(p) < vec2f(2.0));
      return vec4f(0.0, 0.0, 0.0, select(0.0, ramp.x * ramp.y, seen));
    }
  )";
  wgpu::ShaderModuleDescriptor moduleDescriptor{};
  moduleDescriptor.nextInChain = &source;
  moduleDescriptor.label = "VR immersive window mask (multiview)";
  const auto shader = webgpu::g_device.CreateShaderModule(&moduleDescriptor);
  const wgpu::VertexAttribute left[] = {
      {.format = wgpu::VertexFormat::Float32x2, .offset = 0, .shaderLocation = 0},
      {.format = wgpu::VertexFormat::Float32x3, .offset = offsetof(Vertex, window), .shaderLocation = 1},
  };
  const wgpu::VertexAttribute right[] = {
      {.format = wgpu::VertexFormat::Float32x2, .offset = 0, .shaderLocation = 2},
      {.format = wgpu::VertexFormat::Float32x3, .offset = offsetof(Vertex, window), .shaderLocation = 3},
  };
  const wgpu::VertexBufferLayout layouts[] = {
      {.arrayStride = sizeof(Vertex), .attributeCount = 2, .attributes = left},
      {.arrayStride = sizeof(Vertex), .attributeCount = 2, .attributes = right},
  };
  const wgpu::BlendState blend{
      .color = {.operation = wgpu::BlendOperation::Add,
                .srcFactor = wgpu::BlendFactor::Zero,
                .dstFactor = wgpu::BlendFactor::SrcAlpha},
      .alpha = {.operation = wgpu::BlendOperation::Add,
                .srcFactor = wgpu::BlendFactor::One,
                .dstFactor = wgpu::BlendFactor::Zero},
  };
  const wgpu::ColorTargetState color{.format = key.format, .blend = &blend};
  const wgpu::FragmentState fragment{.module = shader, .entryPoint = "fs", .targetCount = 1, .targets = &color};
  const wgpu::DepthStencilState depth{
      .format = key.depthFormat,
      .depthWriteEnabled = false,
      .depthCompare = wgpu::CompareFunction::Always,
      .stencilReadMask = 0,
      .stencilWriteMask = 0,
  };
  wgpu::DawnRenderPipelineMultiview multiview{};
  multiview.viewMask = stereo_multiview::kViewMask;
  wgpu::RenderPipelineDescriptor descriptor{};
  descriptor.nextInChain = &multiview;
  descriptor.label = "VR immersive window mask (multiview)";
  descriptor.vertex = {.module = shader, .entryPoint = "vs", .bufferCount = 2, .buffers = layouts};
  descriptor.fragment = &fragment;
  descriptor.depthStencil = &depth;
  descriptor.multisample.count = key.samples;
  descriptor.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
  slot = {key, webgpu::g_device.CreateRenderPipeline(&descriptor)};
#else
  (void)key;
#endif
  return slot.second;
}

inline const wgpu::RenderPipeline& pipeline(const PipelineKey& key) {
  for (const auto& [cachedKey, cached] : pipelines) {
    if (cached && cachedKey == key) {
      return cached;
    }
  }
  wgpu::ShaderSourceWGSL source{};
  source.code = R"(
    struct Out { @builtin(position) position: vec4f, @location(0) window: vec3f };
    @vertex fn vs(@location(0) position: vec2f, @location(1) window: vec3f) -> Out {
      var o: Out;
      o.position = vec4f(position, 0.5, 1.0);
      o.window = window;
      return o;
    }
    @fragment fn fs(i: Out) -> @location(0) vec4f {
      // Where this pixel's ray meets the screen's plane, in its half extents.
      let p = i.window.xy / i.window.z;
      let edge = 1.0 - abs(p);
      // One pixel of coverage ramp at the edge, from the screen-space rate of change.
      let ramp = clamp(edge / max(fwidth(edge), vec2f(1e-6)) + 0.5, vec2f(0.0), vec2f(1.0));
      // Behind the eye (w <= 0), or far off the screen where the plane nears the horizon.
      let seen = i.window.z > 0.0 && all(abs(p) < vec2f(2.0));
      return vec4f(0.0, 0.0, 0.0, select(0.0, ramp.x * ramp.y, seen));
    }
  )";
  wgpu::ShaderModuleDescriptor moduleDescriptor{};
  moduleDescriptor.nextInChain = &source;
  moduleDescriptor.label = "VR immersive window mask";
  const auto shader = webgpu::g_device.CreateShaderModule(&moduleDescriptor);
  const wgpu::VertexAttribute attributes[] = {
      {.format = wgpu::VertexFormat::Float32x2, .offset = 0, .shaderLocation = 0},
      {.format = wgpu::VertexFormat::Float32x3, .offset = offsetof(Vertex, window), .shaderLocation = 1},
  };
  const wgpu::VertexBufferLayout layout{.arrayStride = sizeof(Vertex), .attributeCount = 2, .attributes = attributes};
  // Premultiplied alpha: the colour is scaled by the coverage the fragment returns as alpha, and the
  // alpha becomes that coverage, whatever the game left there.
  const wgpu::BlendState blend{
      .color = {.operation = wgpu::BlendOperation::Add,
                .srcFactor = wgpu::BlendFactor::Zero,
                .dstFactor = wgpu::BlendFactor::SrcAlpha},
      .alpha = {.operation = wgpu::BlendOperation::Add,
                .srcFactor = wgpu::BlendFactor::One,
                .dstFactor = wgpu::BlendFactor::Zero},
  };
  const wgpu::ColorTargetState color{.format = key.format, .blend = &blend};
  const wgpu::FragmentState fragment{.module = shader, .entryPoint = "fs", .targetCount = 1, .targets = &color};
  const wgpu::DepthStencilState depth{
      .format = key.depthFormat,
      .depthWriteEnabled = false,
      .depthCompare = wgpu::CompareFunction::Always,
      .stencilReadMask = 0,
      .stencilWriteMask = 0,
  };
  wgpu::RenderPipelineDescriptor descriptor{};
  descriptor.label = "VR immersive window mask";
  descriptor.vertex = {.module = shader, .entryPoint = "vs", .bufferCount = 1, .buffers = &layout};
  descriptor.fragment = &fragment;
  descriptor.depthStencil = key.depthFormat == wgpu::TextureFormat::Undefined ? nullptr : &depth;
  descriptor.multisample.count = key.samples;
  descriptor.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
  auto& slot = key.depthFormat == wgpu::TextureFormat::Undefined ? pipelines[1] : pipelines[0];
  slot = {key, webgpu::g_device.CreateRenderPipeline(&descriptor)};
  return slot.second;
}

// Writes the eye's triangle: NDC corners (-1, -1), (3, -1) and (-1, 3) cover the whole image, with
// the mask's rows evaluated at each so they interpolate across it exactly.
inline const wgpu::Buffer& eye_vertices(uint32_t eye, const stereo_replay::WindowMask& mask) {
  static constexpr std::array<std::array<float, 2>, 3> kCorners{{{-1.0f, -1.0f}, {3.0f, -1.0f}, {-1.0f, 3.0f}}};
  std::array<Vertex, 3> vertices{};
  for (size_t i = 0; i < vertices.size(); ++i) {
    const auto h = mask.at(kCorners[i][0], kCorners[i][1]);
    vertices[i] = {{kCorners[i][0], kCorners[i][1]}, {h.x, h.y, h.z}};
  }
  auto& buffer = vertexBuffers[eye];
  if (!buffer) {
    const wgpu::BufferDescriptor descriptor{
        .label = "VR immersive window mask vertices",
        .usage = wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst,
        .size = sizeof(vertices),
    };
    buffer = webgpu::g_device.CreateBuffer(&descriptor);
  }
  webgpu::g_queue.WriteBuffer(buffer, 0, vertices.data(), sizeof(vertices));
  return buffer;
}

// Masks one eye inside a render pass already open on its attachments, after everything else it draws.
inline void draw(const wgpu::RenderPassEncoder& pass, const StereoReplayFrame& frame, uint32_t eye,
                 const stereo_replay::HudScreen& screen) {
  const auto& view = frame.eyes[eye];
  const auto& target = view.target;
  const PipelineKey key{
      .format = webgpu::g_graphicsConfig.surfaceConfiguration.format,
      .depthFormat = target.depthFormat,
      .samples = target.msaaSamples,
  };
  const auto& buffer = eye_vertices(eye, stereo_replay::window_mask(view.projection, view.viewFromCenter, screen));
  pass.SetViewport(0.0f, 0.0f, static_cast<float>(target.size.width), static_cast<float>(target.size.height), 0.0f,
                   1.0f);
  pass.SetScissorRect(0, 0, target.size.width, target.size.height);
  pass.SetPipeline(pipeline(key));
  pass.SetVertexBuffer(0, buffer);
  pass.Draw(3);
}

// Both eyes at once inside a multiview pass over the two-layer targets (StereoReplayFrame::multiview).
inline void draw_multiview(const wgpu::RenderPassEncoder& pass, const StereoReplayFrame& frame,
                           const stereo_replay::HudScreen& screen) {
  const auto& target = frame.eyes[0].target;
  const PipelineKey key{
      .format = webgpu::g_graphicsConfig.surfaceConfiguration.format,
      .depthFormat = target.depthFormat,
      .samples = 1,
  };
  const auto& pipeline = pipeline_multiview(key);
  if (!pipeline) {
    return;
  }
  for (uint32_t eye = 0; eye < AURORA_STEREO_EYE_COUNT; ++eye) {
    const auto& view = frame.eyes[eye];
    eye_vertices(eye, stereo_replay::window_mask(view.projection, view.viewFromCenter, screen));
  }
  pass.SetViewport(0.0f, 0.0f, static_cast<float>(target.size.width), static_cast<float>(target.size.height), 0.0f,
                   1.0f);
  pass.SetScissorRect(0, 0, target.size.width, target.size.height);
  pass.SetPipeline(pipeline);
  pass.SetVertexBuffer(0, vertexBuffers[0]);
  pass.SetVertexBuffer(1, vertexBuffers[1]);
  pass.Draw(3);
}

// The same in a multiview render pass of its own over the finished eyes' layers.
inline void render_multiview(wgpu::CommandEncoder& cmd, const StereoReplayFrame& frame,
                             const stereo_replay::HudScreen& screen) {
#if defined(WGPU_DAWN_RENDER_PASS_MULTIVIEW_INIT)
  const auto& target = frame.eyes[0].target;
  const bool stencil = target.depthFormat == wgpu::TextureFormat::Depth24PlusStencil8;
  const wgpu::RenderPassColorAttachment color{
      .view = frame.multiviewColorView,
      .loadOp = wgpu::LoadOp::Load,
      .storeOp = wgpu::StoreOp::Store,
  };
  const wgpu::RenderPassDepthStencilAttachment depth{
      .view = frame.multiviewDepthView,
      .depthLoadOp = wgpu::LoadOp::Load,
      .depthStoreOp = wgpu::StoreOp::Store,
      .stencilLoadOp = stencil ? wgpu::LoadOp::Load : wgpu::LoadOp::Undefined,
      .stencilStoreOp = stencil ? wgpu::StoreOp::Store : wgpu::StoreOp::Undefined,
  };
  wgpu::DawnRenderPassMultiview multiview{};
  multiview.viewMask = stereo_multiview::kViewMask;
  const wgpu::RenderPassDescriptor descriptor{
      .nextInChain = &multiview,
      .label = "VR immersive window mask (multiview)",
      .colorAttachmentCount = 1,
      .colorAttachments = &color,
      .depthStencilAttachment = &depth,
  };
  const auto pass = cmd.BeginRenderPass(&descriptor);
  draw_multiview(pass, frame, screen);
  pass.End();
#else
  (void)cmd;
  (void)frame;
  (void)screen;
#endif
}

// The same in a render pass of its own, over the finished eye's attachments.
inline void render(wgpu::CommandEncoder& cmd, const StereoReplayFrame& frame, uint32_t eye,
                   const stereo_replay::HudScreen& screen) {
  const auto& target = frame.eyes[eye].target;
  const bool stencil = target.depthFormat == wgpu::TextureFormat::Depth24PlusStencil8;
  const wgpu::RenderPassColorAttachment color{
      .view = target.colorView,
      .resolveTarget = target.resolveView,
      .loadOp = wgpu::LoadOp::Load,
      .storeOp = wgpu::StoreOp::Store,
  };
  const wgpu::RenderPassDepthStencilAttachment depth{
      .view = target.depthView,
      .depthLoadOp = wgpu::LoadOp::Load,
      .depthStoreOp = wgpu::StoreOp::Store,
      .stencilLoadOp = stencil ? wgpu::LoadOp::Load : wgpu::LoadOp::Undefined,
      .stencilStoreOp = stencil ? wgpu::StoreOp::Store : wgpu::StoreOp::Undefined,
  };
  const wgpu::RenderPassDescriptor descriptor{
      .label = "VR immersive window mask",
      .colorAttachmentCount = 1,
      .colorAttachments = &color,
      .depthStencilAttachment = &depth,
  };
  const auto pass = cmd.BeginRenderPass(&descriptor);
  draw(pass, frame, eye, screen);
  pass.End();
}

// Masks an eye image that holds the duplicated mono picture (a frame whose stereo replay could not
// be prepared): only the resolved output exists there, with no depth.
inline void render_output(wgpu::CommandEncoder& cmd, const StereoReplayFrame& frame, uint32_t eye,
                          const wgpu::TextureView& output, wgpu::Extent3D size,
                          const stereo_replay::HudScreen& screen) {
  const auto& view = frame.eyes[eye];
  const PipelineKey key{.format = webgpu::g_graphicsConfig.surfaceConfiguration.format, .samples = 1};
  const auto& buffer = eye_vertices(eye, stereo_replay::window_mask(view.projection, view.viewFromCenter, screen));
  const wgpu::RenderPassColorAttachment color{
      .view = output,
      .loadOp = wgpu::LoadOp::Load,
      .storeOp = wgpu::StoreOp::Store,
  };
  const wgpu::RenderPassDescriptor descriptor{
      .label = "VR immersive window mask",
      .colorAttachmentCount = 1,
      .colorAttachments = &color,
  };
  const auto pass = cmd.BeginRenderPass(&descriptor);
  pass.SetViewport(0.0f, 0.0f, static_cast<float>(size.width), static_cast<float>(size.height), 0.0f, 1.0f);
  pass.SetScissorRect(0, 0, size.width, size.height);
  pass.SetPipeline(pipeline(key));
  pass.SetVertexBuffer(0, buffer);
  pass.Draw(3);
  pass.End();
}

} // namespace aurora::gfx::window_mask
