// SPDX-License-Identifier: GPL-3.0-or-later
// Draws the immersive window's mask (gfx/window_mask.hpp) over a filled eye on a real GPU, in the
// eye's own render pass and in passes of its own, 1x and 4x MSAA, and reads the image back: the
// window keeps its colour with alpha 1, everything outside it becomes transparent black.
#include "../lib/gfx/window_mask.hpp"

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace aurora::webgpu {
wgpu::Device g_device;
wgpu::Queue g_queue;
GraphicsConfig g_graphicsConfig{};
} // namespace aurora::webgpu

namespace {
std::atomic<int> errors = 0;
constexpr uint32_t kSize = 256;

enum class Path { InPass, OwnPass, Output };

struct Pixel {
  int r, g, b, a;
};
} // namespace

int main() {
  using namespace aurora;
  using namespace aurora::webgpu;
  wgpu::InstanceDescriptor instanceDescriptor{};
  const wgpu::InstanceFeatureName timed = wgpu::InstanceFeatureName::TimedWaitAny;
  instanceDescriptor.requiredFeatureCount = 1;
  instanceDescriptor.requiredFeatures = &timed;
  auto instance = wgpu::CreateInstance(&instanceDescriptor);
  wgpu::Adapter adapter;
  wgpu::RequestAdapterOptions options{.backendType = wgpu::BackendType::D3D12};
  auto future = instance.RequestAdapter(&options, wgpu::CallbackMode::WaitAnyOnly,
                                        [&](wgpu::RequestAdapterStatus status, wgpu::Adapter a, wgpu::StringView message) {
                                          if (status == wgpu::RequestAdapterStatus::Success)
                                            adapter = std::move(a);
                                          else
                                            std::cerr << std::string_view(message) << '\n';
                                        });
  if (instance.WaitAny(future, 5000000000) != wgpu::WaitStatus::Success || !adapter)
    return 1;
  wgpu::DeviceDescriptor deviceDescriptor{};
  deviceDescriptor.SetUncapturedErrorCallback([](const wgpu::Device&, wgpu::ErrorType, wgpu::StringView message) {
    ++errors;
    std::cerr << std::string_view(message) << '\n';
  });
  future = adapter.RequestDevice(&deviceDescriptor, wgpu::CallbackMode::WaitAnyOnly,
                                 [&](wgpu::RequestDeviceStatus status, wgpu::Device device, wgpu::StringView message) {
                                   if (status == wgpu::RequestDeviceStatus::Success)
                                     g_device = std::move(device);
                                   else
                                     std::cerr << std::string_view(message) << '\n';
                                 });
  if (instance.WaitAny(future, 5000000000) != wgpu::WaitStatus::Success || !g_device)
    return 1;
  g_queue = g_device.GetQueue();
  g_graphicsConfig.surfaceConfiguration.format = wgpu::TextureFormat::RGBA8Unorm;

  // A 90-degree eye looking at a screen 1 unit ahead, 1 across and 0.5 high: the window covers NDC
  // x in -0.5..0.5 and y in -0.25..0.25, pixels 64..192 across and 96..160 down.
  const gfx::stereo_replay::HudScreen screen{.halfWidth = 0.5f, .halfHeight = 0.25f, .distance = 1.0f};
  for (const Path path : {Path::InPass, Path::OwnPass, Path::Output})
    for (const uint32_t samples : {1u, 4u})
      for (const bool turnedAway : {false, true})
        for (const uint32_t eyeIndex : {0u, 1u}) {
          if (path == Path::Output && samples != 1)
            continue;
          gfx::StereoReplayFrame frame{};
          frame.window = true;
          wgpu::TextureDescriptor textureDescriptor{
              .usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc,
              .size = {kSize, kSize, 1},
              .format = wgpu::TextureFormat::RGBA8Unorm,
              .sampleCount = 1,
          };
          auto output = g_device.CreateTexture(&textureDescriptor);
          textureDescriptor.sampleCount = samples;
          textureDescriptor.usage = wgpu::TextureUsage::RenderAttachment;
          auto color = g_device.CreateTexture(&textureDescriptor);
          textureDescriptor.format = wgpu::TextureFormat::Depth24PlusStencil8;
          auto depth = g_device.CreateTexture(&textureDescriptor);
          auto& eye = frame.eyes[eyeIndex];
          eye.target.colorView = samples == 1 ? output.CreateView() : color.CreateView();
          if (samples > 1)
            eye.target.resolveView = output.CreateView();
          eye.target.depthView = depth.CreateView();
          eye.target.depthFormat = wgpu::TextureFormat::Depth24PlusStencil8;
          eye.target.size = {kSize, kSize, 1};
          eye.target.msaaSamples = samples;
          eye.projection.m0[0] = 1.0f;
          eye.projection.m1[1] = 1.0f;
          eye.viewFromCenter.m0 = {turnedAway ? -1.0f : 1.0f, 0.0f, 0.0f, 0.0f};
          eye.viewFromCenter.m1 = {0.0f, 1.0f, 0.0f, 0.0f};
          eye.viewFromCenter.m2 = {0.0f, 0.0f, turnedAway ? -1.0f : 1.0f, 0.0f};

          // The finished eye, with an alpha the game might leave anywhere.
          auto encoder = g_device.CreateCommandEncoder();
          const wgpu::RenderPassColorAttachment fill{.view = eye.target.colorView,
                                                     .resolveTarget = eye.target.resolveView,
                                                     .loadOp = wgpu::LoadOp::Clear,
                                                     .storeOp = wgpu::StoreOp::Store,
                                                     .clearValue = {0.5, 0.25, 0.75, 0.3}};
          const wgpu::RenderPassDepthStencilAttachment fillDepth{.view = eye.target.depthView,
                                                                 .depthLoadOp = wgpu::LoadOp::Clear,
                                                                 .depthStoreOp = wgpu::StoreOp::Store,
                                                                 .depthClearValue = 1.0f,
                                                                 .stencilLoadOp = wgpu::LoadOp::Clear,
                                                                 .stencilStoreOp = wgpu::StoreOp::Store};
          const wgpu::RenderPassDescriptor fillPass{
              .colorAttachmentCount = 1, .colorAttachments = &fill, .depthStencilAttachment = &fillDepth};
          auto pass = encoder.BeginRenderPass(&fillPass);
          if (path == Path::InPass)
            gfx::window_mask::draw(pass, frame, eyeIndex, screen);
          pass.End();
          if (path == Path::OwnPass)
            gfx::window_mask::render(encoder, frame, eyeIndex, screen);
          if (path == Path::Output)
            gfx::window_mask::render_output(encoder, frame, eyeIndex, output.CreateView(), {kSize, kSize, 1}, screen);

          const wgpu::BufferDescriptor bufferDescriptor{.usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead,
                                                        .size = kSize * kSize * 4};
          auto readback = g_device.CreateBuffer(&bufferDescriptor);
          const wgpu::TexelCopyTextureInfo source{.texture = output};
          const wgpu::TexelCopyBufferInfo destination{.layout = {.bytesPerRow = kSize * 4, .rowsPerImage = kSize},
                                                      .buffer = readback};
          const wgpu::Extent3D extent{kSize, kSize, 1};
          encoder.CopyTextureToBuffer(&source, &destination, &extent);
          auto commands = encoder.Finish();
          g_queue.Submit(1, &commands);
          bool mapped = false;
          future = readback.MapAsync(wgpu::MapMode::Read, 0, kSize * kSize * 4, wgpu::CallbackMode::WaitAnyOnly,
                                     [&](wgpu::MapAsyncStatus status, wgpu::StringView) {
                                       mapped = status == wgpu::MapAsyncStatus::Success;
                                     });
          if (instance.WaitAny(future, 5000000000) != wgpu::WaitStatus::Success || !mapped)
            return 1;
          const auto* bytes = static_cast<const unsigned char*>(readback.GetConstMappedRange());
          const auto at = [&](uint32_t x, uint32_t y) {
            const auto* p = bytes + (y * kSize + x) * 4;
            return Pixel{p[0], p[1], p[2], p[3]};
          };
          const auto near = [](int value, int expected) { return std::abs(value - expected) <= 2; };
          const auto check = [&](uint32_t x, uint32_t y, bool inside) {
            const auto p = at(x, y);
            const bool ok = inside ? near(p.r, 128) && near(p.g, 64) && near(p.b, 191) && p.a == 255
                                   : p.r == 0 && p.g == 0 && p.b == 0 && p.a == 0;
            if (!ok) {
              std::cerr << "path " << static_cast<int>(path) << ", " << samples << "x, eye " << eyeIndex
                        << (turnedAway ? ", turned away" : "") << ": pixel (" << x << ", " << y << ") is (" << p.r
                        << ", " << p.g << ", " << p.b << ", " << p.a << "), expected "
                        << (inside ? "the eye's colour, opaque" : "transparent black") << '\n';
              ++errors;
            }
          };
          const bool seen = !turnedAway;
          check(128, 128, seen);
          check(70, 100, seen);
          check(186, 155, seen);
          check(5, 5, false);
          check(58, 128, false);
          check(198, 128, false);
          check(128, 90, false);
          check(128, 166, false);
          check(250, 250, false);
          readback.Unmap();
          std::cout << "path " << static_cast<int>(path) << ", " << samples << "x MSAA, eye " << eyeIndex
                    << (turnedAway ? ", turned away" : "") << ": checked\n";
        }
  gfx::window_mask::shutdown();
  g_queue = nullptr;
  g_device.Destroy();
  g_device = nullptr;
  return errors ? 1 : 0;
}
