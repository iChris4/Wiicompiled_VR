#include "imgui.hpp"

#include <cstddef>
#include <cmath>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include <webgpu/webgpu_cpp.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_render.h>

#include "fs_helper.hpp"
#include "internal.hpp"
#include "stereo_overlay.hpp"
#include "webgpu/gpu.hpp"
#include "window.hpp"

#define IMGUI_IMPL_WEBGPU_BACKEND_DAWN
#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_sdlrenderer3.h"
#include "backends/imgui_impl_wgpu.h"
#include "tracy/Tracy.hpp"

namespace aurora::imgui {
static float g_scale;
static std::string g_imguiLog{};
static bool g_useSdlRenderer = false;
// Set once ImGui::Render() has produced this frame's draw data. Interpolation encodes up to four
// ImGui passes per frame, and every one of them used to rebuild the draw lists from scratch.
static bool g_frameDataBuilt = true;
// Host-owned frames (see imgui.hpp). Once the host begins one, aurora never calls new_frame() or
// ImGui::Render() itself; the sealed frame carries the host's copy of the draw data instead.
static bool g_hostFrames = false;
static bool g_hostFrameOpen = false;

static std::vector<SDL_Texture*> g_sdlTextures;
static std::vector<wgpu::Texture> g_wgpuTextures;

// Set by the producer, latched by the seal.
static std::mutex g_stereoOverlayMutex;
static StereoOverlay g_stereoOverlay;

void remove_legacy_ini_file(const char* basePath) noexcept {
  if (basePath == nullptr || *basePath == '\0') {
    return;
  }

  std::error_code ec;
  std::filesystem::remove(fs_path_from_string(basePath) / "imgui.ini", ec);
}

void create_context() noexcept {
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
  remove_legacy_ini_file(g_config.userPath);
  remove_legacy_ini_file(g_config.cachePath);
  g_imguiLog = std::string{g_config.cachePath} + "/imgui.log";
  io.IniFilename = nullptr;
  io.LogFilename = g_imguiLog.c_str();
  ImGui::LoadIniSettingsFromMemory("", 0);
  io.WantSaveIniSettings = false;
}

void initialize() noexcept {
  ZoneScoped;
  SDL_Renderer* renderer = window::get_sdl_renderer();
  ImGui_ImplSDL3_InitForSDLRenderer(window::get_sdl_window(), renderer);
  g_useSdlRenderer = renderer != nullptr;
  if (g_useSdlRenderer) {
    ImGui_ImplSDLRenderer3_Init(renderer);
  } else {
    ImGui_ImplWGPU_InitInfo info;
    info.Device = webgpu::g_device.Get();
    info.RenderTargetFormat = static_cast<WGPUTextureFormat>(webgpu::g_graphicsConfig.surfaceConfiguration.format);
    // Interpolation records up to four ImGui passes in one command buffer, so keep three logical
    // frames of renderer resources to stop them overwriting each other's vertex/index buffers.
    info.NumFramesInFlight = 12;
    ImGui_ImplWGPU_Init(&info);
  }
}

void shutdown() noexcept {
  ZoneScoped;
  // Startup can fail before either backend initializes. A context alone does
  // not mean its renderer/platform backend owns resources to release.
  if (ImGui::GetCurrentContext() != nullptr) {
    ImGuiIO& io = ImGui::GetIO();
    if (io.BackendRendererUserData != nullptr) {
      if (g_useSdlRenderer) {
        ImGui_ImplSDLRenderer3_Shutdown();
      } else {
        ImGui_ImplWGPU_Shutdown();
      }
    }
    if (io.BackendPlatformUserData != nullptr) {
      ImGui_ImplSDL3_Shutdown();
    }
    ImGui::DestroyContext();
  }
  for (const auto& texture : g_sdlTextures) {
    SDL_DestroyTexture(texture);
  }
  g_sdlTextures.clear();
  g_wgpuTextures.clear();
  g_useSdlRenderer = false;
  g_scale = 0.f;
  g_frameDataBuilt = false;
}

void process_event(const SDL_Event& event) noexcept {
  auto renderEvent = event;
  if (g_useSdlRenderer) {
    if (SDL_Renderer* renderer = window::get_sdl_renderer()) {
      SDL_ConvertEventToRenderCoordinates(renderer, &renderEvent);
    }
  }
  ImGui_ImplSDL3_ProcessEvent(&renderEvent);
}

bool wants_capture_event(const SDL_Event& event) noexcept {
  if (ImGui::GetCurrentContext() == nullptr) {
    return false;
  }

  const ImGuiIO& io = ImGui::GetIO();
  switch (event.type) {
  case SDL_EVENT_MOUSE_MOTION:
  case SDL_EVENT_MOUSE_BUTTON_DOWN:
  case SDL_EVENT_MOUSE_BUTTON_UP:
  case SDL_EVENT_MOUSE_WHEEL:
  case SDL_EVENT_FINGER_DOWN:
  case SDL_EVENT_FINGER_MOTION:
  case SDL_EVENT_FINGER_UP:
  case SDL_EVENT_FINGER_CANCELED:
    return io.WantCaptureMouse;
  case SDL_EVENT_KEY_DOWN:
  case SDL_EVENT_KEY_UP:
  case SDL_EVENT_TEXT_INPUT:
    return io.WantCaptureKeyboard || io.WantTextInput;
  default:
    return false;
  }
}

void new_frame(const AuroraWindowSize& size) noexcept {
  ZoneScoped;
  ImVec2 framebufferScale{
      size.width > 0 ? static_cast<float>(size.native_fb_width) / static_cast<float>(size.width) : 1.0f,
      size.height > 0 ? static_cast<float>(size.native_fb_height) / static_cast<float>(size.height) : 1.0f,
  };
  ImVec2 displaySize{static_cast<float>(size.width), static_cast<float>(size.height)};

  if (g_useSdlRenderer) {
    if (SDL_Renderer* renderer = window::get_sdl_renderer()) {
      float renderScaleX = 1.0f;
      float renderScaleY = 1.0f;
      SDL_GetRenderScale(renderer, &renderScaleX, &renderScaleY);
      if (renderScaleX > 0.0f && renderScaleY > 0.0f &&
          (std::fabs(renderScaleX - 1.0f) > 0.0001f || std::fabs(renderScaleY - 1.0f) > 0.0001f)) {
        int outputWidth = static_cast<int>(size.native_fb_width);
        int outputHeight = static_cast<int>(size.native_fb_height);
        SDL_GetRenderOutputSize(renderer, &outputWidth, &outputHeight);
        displaySize = {
            static_cast<float>(outputWidth) / renderScaleX,
            static_cast<float>(outputHeight) / renderScaleY,
        };
        framebufferScale = {renderScaleX, renderScaleY};
      }
    }
    ImGui_ImplSDLRenderer3_NewFrame();
    g_scale = size.scale;
  } else {
    if (g_scale != size.scale) {
      if (g_scale > 0.f) {
        ImGui_ImplWGPU_CreateDeviceObjects();
      }
      g_scale = size.scale;
    }
    if (!ImGui::GetIO().Fonts->IsBuilt()) {
      ImGui_ImplWGPU_CreateDeviceObjects();
    }
    ImGui_ImplWGPU_NewFrame();
  }
  ImGui_ImplSDL3_NewFrame();

  ImGuiIO& io = ImGui::GetIO();
  io.DisplayFramebufferScale = framebufferScale;
  ImGui::GetIO().DisplaySize = displaySize;
  ImGui::NewFrame();
  g_frameDataBuilt = false;
}

void render_frame_data() noexcept {
  ZoneScoped;
  if (g_frameDataBuilt || g_hostFrames) {
    return;
  }
  ImGui::Render();
  auto* data = ImGui::GetDrawData();
  data->FramebufferScale = ImGui::GetIO().DisplayFramebufferScale;
  g_frameDataBuilt = true;
}

void render(const wgpu::RenderPassEncoder& pass) noexcept {
  ZoneScoped;
  if (g_hostFrames) {
    // The shared context's draw data belongs to the host's current frame now;
    // a sealed frame without a host copy has nothing safe to draw.
    return;
  }
  render_frame_data();

  auto* data = ImGui::GetDrawData();
  if (g_useSdlRenderer) {
    SDL_Renderer* renderer = window::get_sdl_renderer();
    SDL_RenderClear(renderer);
    ImGui_ImplSDLRenderer3_RenderDrawData(data, renderer);
    SDL_RenderPresent(renderer);
  } else {
    pass.PushDebugGroup("Aurora: Dear Imgui");
    ImGui_ImplWGPU_RenderDrawData(data, pass.Get());
    pass.PopDebugGroup();
  }
}

struct HostFrame {
  ImDrawData data{};
  std::vector<ImDrawList*> lists;
  ~HostFrame() {
    for (ImDrawList* list : lists) {
      IM_DELETE(list);
    }
  }
};

void host_frame_begin(const AuroraWindowSize& size) noexcept {
  g_hostFrames = true;
  if (g_hostFrameOpen) {
    return;
  }
  if (!g_frameDataBuilt) {
    // aurora started this frame itself before the host took over: adopt it.
    g_hostFrameOpen = true;
    return;
  }
  new_frame(size);
  g_hostFrameOpen = true;
}

HostFramePtr host_frame_end() noexcept {
  ZoneScoped;
  if (!g_hostFrameOpen) {
    host_frame_begin(window::get_window_size());
  }
  ImGui::Render();
  ImDrawData* source = ImGui::GetDrawData();
  source->FramebufferScale = ImGui::GetIO().DisplayFramebufferScale;
  auto frame = std::make_shared<HostFrame>();
  frame->data = *source;
  frame->data.CmdLists.clear();
  frame->lists.reserve(static_cast<size_t>(source->CmdListsCount));
  for (int i = 0; i < source->CmdListsCount; ++i) {
    const ImDrawList* src = source->CmdLists[i];
    ImDrawList* copy = IM_NEW(ImDrawList)(src->_Data);
    copy->CmdBuffer = src->CmdBuffer;
    copy->IdxBuffer = src->IdxBuffer;
    copy->VtxBuffer = src->VtxBuffer;
    copy->Flags = src->Flags;
    frame->lists.push_back(copy);
    frame->data.CmdLists.push_back(copy);
  }
  g_hostFrameOpen = false;
  g_frameDataBuilt = true;
  return frame;
}

bool host_frames_active() noexcept { return g_hostFrames; }

const ImDrawData* host_frame_draw_data(const HostFrame& frame) noexcept { return &frame.data; }

void render(const wgpu::RenderPassEncoder& pass, const ImDrawData* data) noexcept {
  ZoneScoped;
  if (g_useSdlRenderer || data == nullptr) {
    return;
  }
  pass.PushDebugGroup("Aurora: Dear Imgui");
  ImGui_ImplWGPU_RenderDrawData(const_cast<ImDrawData*>(data), pass.Get());
  pass.PopDebugGroup();
}

StereoOverlay latch_stereo_overlay() noexcept {
  std::lock_guard lock(g_stereoOverlayMutex);
  return g_stereoOverlay;
}

bool render_draw_data(const wgpu::RenderPassEncoder& pass, ImDrawData* data) noexcept {
  ZoneScoped;
  // The SDL renderer fallback has no render passes to draw into.
  if (g_useSdlRenderer || data == nullptr || ImGui::GetCurrentContext() == nullptr) {
    return false;
  }
  pass.PushDebugGroup("Aurora: Dear Imgui headset panel");
  ImGui_ImplWGPU_RenderDrawData(data, pass.Get());
  pass.PopDebugGroup();
  return true;
}

ImTextureID add_texture(uint32_t width, uint32_t height, const uint8_t* data) noexcept {
  if (SDL_Renderer* renderer = window::get_sdl_renderer()) {
    SDL_Texture* texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, width, height);
    SDL_UpdateTexture(texture, nullptr, data, width * 4);
    SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
    g_sdlTextures.push_back(texture);
    return reinterpret_cast<ImTextureID>(texture);
  }
  const wgpu::Extent3D size{
      .width = width,
      .height = height,
      .depthOrArrayLayers = 1,
  };
  const wgpu::TextureDescriptor textureDescriptor{
      .label = "imgui texture",
      .usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst,
      .dimension = wgpu::TextureDimension::e2D,
      .size = size,
      .format = wgpu::TextureFormat::RGBA8Unorm,
      .mipLevelCount = 1,
      .sampleCount = 1,
  };
  const wgpu::TextureViewDescriptor textureViewDescriptor{
      .label = "imgui texture view",
      .format = wgpu::TextureFormat::RGBA8Unorm,
      .dimension = wgpu::TextureViewDimension::e2D,
      .mipLevelCount = WGPU_MIP_LEVEL_COUNT_UNDEFINED,
      .arrayLayerCount = WGPU_ARRAY_LAYER_COUNT_UNDEFINED,
  };
  auto texture = webgpu::g_device.CreateTexture(&textureDescriptor);
  auto textureView = texture.CreateView(&textureViewDescriptor);
  {
    const wgpu::TexelCopyTextureInfo dstView{
        .texture = texture,
    };
    const wgpu::TexelCopyBufferLayout dataLayout{
        .bytesPerRow = 4 * width,
        .rowsPerImage = height,
    };
    webgpu::g_queue.WriteTexture(&dstView, data, width * height * 4, &dataLayout, &size);
  }
  g_wgpuTextures.push_back(texture);
  return reinterpret_cast<ImTextureID>(textureView.MoveToCHandle());
}
} // namespace aurora::imgui

// C bindings
extern "C" {
ImTextureID aurora_imgui_add_texture(uint32_t width, uint32_t height, const void* rgba8) {
  return aurora::imgui::add_texture(width, height, static_cast<const uint8_t*>(rgba8));
}

void aurora_set_stereo_panel_layer(bool enabled) { aurora::stereo_overlay::set_layer_mode(enabled); }

void aurora_imgui_set_stereo_overlay(ImDrawData* drawData, float widthFraction) {
  std::lock_guard lock(aurora::imgui::g_stereoOverlayMutex);
  aurora::imgui::g_stereoOverlay = {
      .drawData = drawData,
      .widthFraction = drawData != nullptr ? widthFraction : 0.f,
  };
}
}
