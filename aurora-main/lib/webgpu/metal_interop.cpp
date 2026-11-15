#include <aurora/metal_interop.h>

#include "../internal.hpp"
#include "../stereo.hpp"
#include "../stereo_overlay.hpp"
#include "gpu.hpp"

#if defined(__APPLE__) && defined(WEBGPU_DAWN) && defined(DAWN_ENABLE_BACKEND_METAL)

#include <CoreFoundation/CoreFoundation.h>
#include <IOSurface/IOSurfaceRef.h>
#include <magic_enum.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>

// The IOSurface twin of vulkan_interop.cpp. Same shape, same protocol: the
// pacing thread publishes targets, the frame worker imports and copies, the
// submit hook ends the accesses and hands the compositor side Dawn's release
// fences. Nothing here calls Metal directly: IOSurfaceRef is a CoreFoundation
// object and an MTLSharedEvent only ever passes through as a retained pointer.
namespace aurora::metal_interop {
namespace {

Module Log("aurora::metal_interop");

int64_t to_mtl_format(wgpu::TextureFormat format) noexcept {
  switch (format) {
  case wgpu::TextureFormat::RGBA8Unorm:
    return AURORA_MTL_PIXEL_FORMAT_RGBA8_UNORM;
  case wgpu::TextureFormat::RGBA8UnormSrgb:
    return AURORA_MTL_PIXEL_FORMAT_RGBA8_UNORM_SRGB;
  case wgpu::TextureFormat::BGRA8Unorm:
    return AURORA_MTL_PIXEL_FORMAT_BGRA8_UNORM;
  case wgpu::TextureFormat::BGRA8UnormSrgb:
    return AURORA_MTL_PIXEL_FORMAT_BGRA8_UNORM_SRGB;
  case wgpu::TextureFormat::RGBA16Float:
    return AURORA_MTL_PIXEL_FORMAT_RGBA16_FLOAT;
  default:
    return AURORA_MTL_PIXEL_FORMAT_INVALID;
  }
}

// CopyTextureToTexture wants the same format modulo sRGB encoding, and an
// IOSurface has no encoding of its own: the compositor side declares the sRGB
// sibling on its texture view, so only the copy family has to agree here.
int copy_family(int64_t format) noexcept {
  switch (format) {
  case AURORA_MTL_PIXEL_FORMAT_RGBA8_UNORM:
  case AURORA_MTL_PIXEL_FORMAT_RGBA8_UNORM_SRGB:
    return 1;
  case AURORA_MTL_PIXEL_FORMAT_BGRA8_UNORM:
  case AURORA_MTL_PIXEL_FORMAT_BGRA8_UNORM_SRGB:
    return 2;
  case AURORA_MTL_PIXEL_FORMAT_RGBA16_FLOAT:
    return 3;
  default:
    return 0;
  }
}

bool same_copy_family(int64_t left, int64_t right) noexcept {
  const int family = copy_family(left);
  return family != 0 && family == copy_family(right);
}

bool device_supports_bridge() noexcept {
  return webgpu::g_device && webgpu::g_backendType == wgpu::BackendType::Metal &&
         webgpu::g_device.HasFeature(wgpu::FeatureName::SharedTextureMemoryIOSurface) &&
         webgpu::g_device.HasFeature(wgpu::FeatureName::SharedFenceMTLSharedEvent);
}

void release_event(void*& event) noexcept {
  if (event != nullptr) {
    CFRelease(static_cast<CFTypeRef>(event));
  }
  event = nullptr;
}

// One IOSurface imported into Dawn. Created on first use and kept until the
// bridge is disabled; the ring the compositor side recycles is a handful of
// surfaces per swapchain, so this never grows beyond a few dozen entries.
struct Import {
  IOSurfaceRef surface = nullptr;
  wgpu::SharedTextureMemory memory;
  wgpu::Texture texture;
  wgpu::TextureFormat format = wgpu::TextureFormat::Undefined;
  uint32_t width = 0;
  uint32_t height = 0;
  bool initialized = false;
  bool accessBegun = false;
};

// The eyes, then the settings panel's layer image in a slot of its own.
constexpr uint32_t kPanelIndex = AURORA_METAL_STEREO_MAX_TARGETS;
constexpr uint32_t kMaxImages = AURORA_METAL_STEREO_MAX_RELEASES;
using Releases = std::array<AuroraMetalStereoRelease, kMaxImages>;

struct PendingTarget {
  IOSurfaceRef surface = nullptr;
  uint32_t width = 0;
  uint32_t height = 0;
  int64_t metalPixelFormat = AURORA_MTL_PIXEL_FORMAT_INVALID;
  void* acquireEvent = nullptr; // retained while pending
  uint64_t acquireValue = 0;
};

class StereoBridge final {
public:
  StereoBridge(AuroraMetalStereoSubmittedCallback callback, void* userdata) noexcept
      : m_callback(callback), m_userdata(userdata) {}

  ~StereoBridge() { ReleaseImportsLocked(); }

  bool Initialize() noexcept {
    if (!device_supports_bridge()) {
      Log.error("Dawn Metal device lacks SharedTextureMemoryIOSurface or SharedFenceMTLSharedEvent");
      return false;
    }
    m_auroraFormat = webgpu::g_graphicsConfig.surfaceConfiguration.format;
    return to_mtl_format(m_auroraFormat) != AURORA_MTL_PIXEL_FORMAT_INVALID;
  }

  bool PrepareForDestruction() noexcept {
    std::lock_guard lock(m_mutex);
    // Dawn's queue is drained by aurora_quiesce_frame_worker() before this is
    // reached; all that can be outstanding here is a begun access whose
    // EndAccess never ran because the frame was abandoned.
    for (auto& [surface, import] : m_imports) {
      if (import.accessBegun) {
        wgpu::SharedTextureMemoryEndAccessState end{};
        import.memory.EndAccess(import.texture, &end);
        import.accessBegun = false;
      }
    }
    ReleaseImportsLocked();
    return true;
  }

  bool SetTargets(uint64_t token, const AuroraMetalStereoTarget* targets, uint32_t targetCount,
                  const AuroraMetalStereoTarget* panel) noexcept {
    if (token == 0 || targets == nullptr || targetCount == 0 || targetCount > AURORA_METAL_STEREO_MAX_TARGETS) {
      return false;
    }
    std::lock_guard lock(m_mutex);
    if (m_framePending || m_encoded) {
      return false;
    }
    const int64_t auroraFormat = to_mtl_format(m_auroraFormat);
    const auto valid = [&](const AuroraMetalStereoTarget& target) {
      return target.ioSurface != nullptr && target.width != 0 && target.height != 0 &&
             same_copy_family(target.metalPixelFormat, auroraFormat);
    };
    for (uint32_t eye = 0; eye < targetCount; ++eye) {
      if (!valid(targets[eye])) {
        return false;
      }
    }
    if (panel != nullptr && !valid(*panel)) {
      return false;
    }
    m_targets = {};
    m_imageCount = 0;
    const auto add = [&](uint32_t index, const AuroraMetalStereoTarget& target) {
      if (target.acquireEvent != nullptr) {
        CFRetain(static_cast<CFTypeRef>(target.acquireEvent));
      }
      m_targets[index] = {
          .surface = static_cast<IOSurfaceRef>(target.ioSurface),
          .width = target.width,
          .height = target.height,
          .metalPixelFormat = target.metalPixelFormat,
          .acquireEvent = target.acquireEvent,
          .acquireValue = target.acquireValue,
      };
      m_images[m_imageCount++] = index;
    };
    for (uint32_t eye = 0; eye < targetCount; ++eye) {
      add(eye, targets[eye]);
    }
    if (panel != nullptr) {
      add(kPanelIndex, *panel);
    }
    m_frameToken = token;
    m_targetCount = targetCount;
    m_framePending = true;
    return true;
  }

  bool Encode(wgpu::CommandEncoder& encoder, const stereo::SinkFrame& frame) noexcept {
    std::lock_guard lock(m_mutex);
    if (!m_framePending || m_encoded || frame.frameToken != m_frameToken) {
      return false;
    }
    if (EncodeLocked(encoder, frame)) {
      m_encoded = true;
      return true;
    }
    // Nothing was recorded, so the shared surfaces are untouched.
    PublishAndClearFrameLocked(frame.frameToken, false, false);
    return false;
  }

  void Submitted(const stereo::SinkFrame& frame) noexcept {
    std::lock_guard lock(m_mutex);
    if (!m_framePending || !m_encoded || frame.frameToken != m_frameToken) {
      return;
    }
    Releases releases{};
    const bool success = EndAccessLocked(releases);
    NotifyLocked(frame.frameToken, success, true, releases);
    ClearFrameLocked();
  }

  void CancelPending() noexcept {
    std::lock_guard lock(m_mutex);
    if (!m_framePending) {
      return;
    }
    const uint64_t token = m_frameToken;
    const bool encoded = m_encoded;
    Releases releases{};
    if (encoded) {
      EndAccessLocked(releases);
      for (auto& release : releases) {
        release_event(release.releaseEvent);
      }
    }
    PublishAndClearFrameLocked(token, false, encoded);
  }

  bool CancelBeforeEncode(uint64_t token) noexcept {
    std::unique_lock lock(m_mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
      return false;
    }
    if (token == 0 || !m_framePending || m_encoded || token != m_frameToken) {
      return false;
    }
    ClearFrameLocked();
    return true;
  }

private:
  // An eye may be smaller than its surface (the immersive window's eyes are the window only): it is
  // copied into the surface's top-left corner, and the compositor side shows just that rectangle.
  Import* EnsureImport(uint32_t eye, const stereo::EyeImage& source) noexcept {
    const auto& target = m_targets[eye];
    if (source.texture == nullptr || source.format != m_auroraFormat || source.size.width == 0 ||
        source.size.height == 0 || source.size.width > target.width || source.size.height > target.height) {
      Log.error("Stereo image {} does not fit its IOSurface target ({}x{} in {}x{})", eye, source.size.width,
                source.size.height, target.width, target.height);
      return nullptr;
    }
    if (auto found = m_imports.find(target.surface); found != m_imports.end()) {
      auto& import = found->second;
      if (import.width == target.width && import.height == target.height && import.format == source.format) {
        return &import;
      }
      Log.error("IOSurface for eye {} was re-used with a different geometry", eye);
      return nullptr;
    }

    Import import;
    import.surface = target.surface;
    CFRetain(import.surface);

    wgpu::SharedTextureMemoryIOSurfaceDescriptor ioSurface{};
    ioSurface.ioSurface = target.surface;
    ioSurface.allowStorageBinding = false;
    const wgpu::SharedTextureMemoryDescriptor memoryDescriptor{
        .nextInChain = &ioSurface,
        .label = eye == 0   ? "OpenXR left eye IOSurface"
                 : eye == 1 ? "OpenXR right eye IOSurface"
                            : "OpenXR panel IOSurface",
    };
    import.memory = webgpu::g_device.ImportSharedTextureMemory(&memoryDescriptor);
    if (!import.memory) {
      Log.error("Dawn rejected the IOSurface import for eye {}", eye);
      CFRelease(import.surface);
      return nullptr;
    }
    wgpu::SharedTextureMemoryProperties properties{};
    if (import.memory.GetProperties(&properties) != wgpu::Status::Success ||
        properties.size.width != target.width || properties.size.height != target.height ||
        (properties.usage & wgpu::TextureUsage::CopyDst) == wgpu::TextureUsage::None) {
      Log.error("Dawn reported incompatible IOSurface properties for eye {}", eye);
      CFRelease(import.surface);
      return nullptr;
    }
    if (properties.format != source.format) {
      // The compositor side allocates its surfaces in Aurora's own format
      // (aurora_metal_get_native_handles), so this only happens when the two
      // disagree about it, where a copy could never be legal anyway.
      Log.error("IOSurface format {} does not match Aurora's {} for eye {}", magic_enum::enum_name(properties.format),
                magic_enum::enum_name(source.format), eye);
      CFRelease(import.surface);
      return nullptr;
    }
    const wgpu::TextureDescriptor textureDescriptor{
        .label = eye == 0   ? "OpenXR left eye shared texture"
                 : eye == 1 ? "OpenXR right eye shared texture"
                            : "OpenXR panel shared texture",
        .usage = wgpu::TextureUsage::CopyDst,
        .dimension = wgpu::TextureDimension::e2D,
        .size = {target.width, target.height, 1},
        .format = source.format,
        .mipLevelCount = 1,
        .sampleCount = 1,
    };
    import.texture = import.memory.CreateTexture(&textureDescriptor);
    if (!import.texture) {
      Log.error("Dawn could not wrap the IOSurface for eye {}", eye);
      CFRelease(import.surface);
      return nullptr;
    }
    import.format = source.format;
    import.width = target.width;
    import.height = target.height;
    const auto [inserted, ok] = m_imports.emplace(target.surface, std::move(import));
    return ok ? &inserted->second : nullptr;
  }

  bool EncodeLocked(wgpu::CommandEncoder& encoder, const stereo::SinkFrame& frame) noexcept {
    std::array<stereo::EyeImage, kMaxImages> sources{};
    std::array<Import*, kMaxImages> imports{};
    for (uint32_t n = 0; n < m_imageCount; ++n) {
      const uint32_t eye = m_images[n];
      if (eye == kPanelIndex) {
        if (!stereo_overlay::layer_source(encoder, m_targets[eye].width, m_targets[eye].height, sources[eye])) {
          return false;
        }
      } else {
        sources[eye] = frame.eyes[eye];
      }
      imports[eye] = EnsureImport(eye, sources[eye]);
      if (imports[eye] == nullptr) {
        return false;
      }
    }
    for (uint32_t n = 0; n < m_imageCount; ++n) {
      const uint32_t eye = m_images[n];
      auto& target = m_targets[eye];
      auto& import = *imports[eye];
      if (import.accessBegun) {
        Log.error("IOSurface for eye {} is still under a previous access", eye);
        RollbackAccesses(imports, n);
        return false;
      }
      // The compositor side's copy out of this surface signals its shared event;
      // Dawn waits for that value on its queue before the copy in. A surface never
      // written has undefined contents, which Dawn treats as uninitialized.
      wgpu::SharedFence acquireFence;
      if (target.acquireEvent != nullptr) {
        wgpu::SharedFenceMTLSharedEventDescriptor sharedEvent{};
        sharedEvent.sharedEvent = target.acquireEvent;
        const wgpu::SharedFenceDescriptor fenceDescriptor{
            .nextInChain = &sharedEvent,
            .label = "OpenXR eye copy-out event",
        };
        acquireFence = webgpu::g_device.ImportSharedFence(&fenceDescriptor);
        if (!acquireFence) {
          Log.error("Dawn could not import the compositor's copy-out event for eye {}", eye);
          RollbackAccesses(imports, n);
          return false;
        }
      }
      const std::array fences{acquireFence};
      const std::array<uint64_t, 1> values{target.acquireValue};
      wgpu::SharedTextureMemoryBeginAccessDescriptor begin{};
      begin.concurrentRead = false;
      begin.initialized = import.initialized;
      if (acquireFence) {
        begin.fenceCount = 1;
        begin.fences = fences.data();
        begin.signaledValueCount = 1;
        begin.signaledValues = values.data();
      }
      if (import.memory.BeginAccess(import.texture, &begin) != wgpu::Status::Success) {
        Log.error("Dawn BeginAccess failed for stereo eye {}", eye);
        RollbackAccesses(imports, n);
        return false;
      }
      import.accessBegun = true;
      // Dawn keeps what it needs of the event; the bridge's own reference goes now.
      release_event(target.acquireEvent);
    }
    for (uint32_t n = 0; n < m_imageCount; ++n) {
      const uint32_t eye = m_images[n];
      const auto& import = *imports[eye];
      const wgpu::TexelCopyTextureInfo source{
          .texture = *sources[eye].texture,
          .mipLevel = 0,
          .origin = {},
          .aspect = wgpu::TextureAspect::All,
      };
      const wgpu::TexelCopyTextureInfo destination{
          .texture = import.texture,
          .mipLevel = 0,
          .origin = {},
          .aspect = wgpu::TextureAspect::All,
      };
      const wgpu::Extent3D extent{sources[eye].size.width, sources[eye].size.height, 1};
      encoder.CopyTextureToTexture(&source, &destination, &extent);
      m_encodedImports[eye] = imports[eye];
    }
    return true;
  }

  // Ends the accesses begun for the first `count` images of this frame.
  void RollbackAccesses(const std::array<Import*, kMaxImages>& imports, uint32_t count) noexcept {
    for (uint32_t n = 0; n < count; ++n) {
      const uint32_t eye = m_images[n];
      if (imports[eye] != nullptr && imports[eye]->accessBegun) {
        wgpu::SharedTextureMemoryEndAccessState end{};
        imports[eye]->memory.EndAccess(imports[eye]->texture, &end);
        imports[eye]->initialized = end.initialized;
        imports[eye]->accessBegun = false;
      }
    }
    for (auto& target : m_targets) {
      release_event(target.acquireEvent);
    }
  }

  // Fills one release per image of this frame, in order: the eyes, then the panel.
  bool EndAccessLocked(Releases& releases) noexcept {
    bool success = true;
    for (auto& release : releases) {
      release = {.releaseEvent = nullptr, .releaseValue = 0};
    }
    for (uint32_t n = 0; n < m_imageCount; ++n) {
      const uint32_t eye = m_images[n];
      auto& release = releases[n];
      Import* import = m_encodedImports[eye];
      if (import == nullptr || !import->accessBegun) {
        success = false;
        continue;
      }
      wgpu::SharedTextureMemoryEndAccessState end{};
      if (import->memory.EndAccess(import->texture, &end) != wgpu::Status::Success) {
        Log.error("Dawn EndAccess failed for stereo eye {}", eye);
        success = false;
      } else {
        import->initialized = end.initialized;
        for (size_t i = 0; i < end.fenceCount; ++i) {
          wgpu::SharedFenceMTLSharedEventExportInfo sharedEvent{};
          wgpu::SharedFenceExportInfo info{};
          info.nextInChain = &sharedEvent;
          end.fences[i].ExportInfo(&info);
          if (info.type == wgpu::SharedFenceType::MTLSharedEvent && sharedEvent.sharedEvent != nullptr) {
            if (release.releaseEvent != nullptr) {
              // Dawn's Metal backend returns one fence per access. Should it ever
              // return two, the later value on the same event is the one to wait for
              // when the events match; different events cannot be merged here.
              if (release.releaseEvent == sharedEvent.sharedEvent) {
                release.releaseValue = std::max(release.releaseValue, end.signaledValues[i]);
                continue;
              }
              Log.warn("Dawn returned several release events for eye {}; keeping the last", eye);
              release_event(release.releaseEvent);
            }
            CFRetain(static_cast<CFTypeRef>(sharedEvent.sharedEvent));
            release.releaseEvent = sharedEvent.sharedEvent;
            release.releaseValue = end.signaledValues[i];
          } else {
            Log.error("Dawn returned a non-MTLSharedEvent fence for eye {}", eye);
            success = false;
          }
        }
      }
      import->accessBegun = false;
    }
    return success;
  }

public:
  bool ForgetTargets(void* const* surfaces, uint32_t count) noexcept {
    std::lock_guard lock(m_mutex);
    if (m_framePending) {
      return false;
    }
    for (uint32_t i = 0; i < count; ++i) {
      const auto it = m_imports.find(static_cast<IOSurfaceRef>(surfaces[i]));
      if (it == m_imports.end()) {
        continue;
      }
      if (it->second.accessBegun) {
        return false;
      }
      it->second.texture = nullptr;
      it->second.memory = nullptr;
      if (it->second.surface != nullptr) {
        CFRelease(it->second.surface);
      }
      m_imports.erase(it);
    }
    return true;
  }

private:
  void ReleaseImportsLocked() noexcept {
    for (auto& [surface, import] : m_imports) {
      import.texture = nullptr;
      import.memory = nullptr;
      if (import.surface != nullptr) {
        CFRelease(import.surface);
      }
    }
    m_imports.clear();
  }

  void ClearFrameLocked() noexcept {
    for (auto& target : m_targets) {
      release_event(target.acquireEvent);
      target = {};
    }
    m_encodedImports = {};
    m_frameToken = 0;
    m_targetCount = 0;
    m_imageCount = 0;
    m_framePending = false;
    m_encoded = false;
  }

  void PublishAndClearFrameLocked(uint64_t token, bool success, bool gpuWorkQueued) noexcept {
    Releases releases{};
    NotifyLocked(token, success, gpuWorkQueued, releases);
    ClearFrameLocked();
  }

  void NotifyLocked(uint64_t token, bool success, bool gpuWorkQueued, const Releases& releases) noexcept {
    if (m_callback != nullptr) {
      m_callback(token, success, gpuWorkQueued, releases.data(), m_imageCount, m_userdata);
    } else {
      for (auto release : releases) {
        release_event(release.releaseEvent);
      }
    }
  }

  std::mutex m_mutex;
  std::unordered_map<IOSurfaceRef, Import> m_imports;
  std::array<PendingTarget, kMaxImages> m_targets{};
  std::array<Import*, kMaxImages> m_encodedImports{};
  // The slots of m_targets this frame copies into, eyes first.
  std::array<uint32_t, kMaxImages> m_images{};
  uint32_t m_imageCount = 0;
  wgpu::TextureFormat m_auroraFormat = wgpu::TextureFormat::Undefined;
  AuroraMetalStereoSubmittedCallback m_callback = nullptr;
  void* m_userdata = nullptr;
  uint64_t m_frameToken = 0;
  uint32_t m_targetCount = 0;
  bool m_framePending = false;
  bool m_encoded = false;
};

std::unique_ptr<StereoBridge> g_bridge;

bool sink_encode(wgpu::CommandEncoder& encoder, const stereo::SinkFrame& frame, void* userdata) noexcept {
  return static_cast<StereoBridge*>(userdata)->Encode(encoder, frame);
}

void sink_submitted(const stereo::SinkFrame& frame, void* userdata) noexcept {
  static_cast<StereoBridge*>(userdata)->Submitted(frame);
}

} // namespace
} // namespace aurora::metal_interop

bool aurora_metal_get_native_handles(AuroraMetalNativeHandles* handles) {
  if (handles == nullptr) {
    return false;
  }
  *handles = {};
  using namespace aurora::metal_interop;
  if (!aurora::webgpu::g_device || aurora::webgpu::g_backendType != wgpu::BackendType::Metal) {
    return false;
  }
  const int64_t format = to_mtl_format(aurora::webgpu::g_graphicsConfig.surfaceConfiguration.format);
  if (format == AURORA_MTL_PIXEL_FORMAT_INVALID) {
    return false;
  }
  *handles = {
      .colorMetalPixelFormat = format,
      .sharedTextureMemoryIOSurface =
          aurora::webgpu::g_device.HasFeature(wgpu::FeatureName::SharedTextureMemoryIOSurface),
      .sharedFenceMTLSharedEvent = aurora::webgpu::g_device.HasFeature(wgpu::FeatureName::SharedFenceMTLSharedEvent),
  };
  return true;
}

bool aurora_metal_enable_stereo_bridge(AuroraMetalStereoSubmittedCallback submitted, void* userdata) {
  using namespace aurora::metal_interop;
  if (g_bridge || submitted == nullptr) {
    return false;
  }
  auto bridge = std::make_unique<StereoBridge>(submitted, userdata);
  if (!bridge->Initialize()) {
    return false;
  }
  aurora::stereo::set_sink(sink_encode, sink_submitted, bridge.get());
  g_bridge = std::move(bridge);
  return true;
}

bool aurora_metal_set_stereo_targets(uint64_t frameToken, const AuroraMetalStereoTarget* targets,
                                     uint32_t targetCount) {
  using namespace aurora::metal_interop;
  return g_bridge && g_bridge->SetTargets(frameToken, targets, targetCount, nullptr);
}

bool aurora_metal_set_stereo_targets_with_panel(uint64_t frameToken, const AuroraMetalStereoTarget* targets,
                                                uint32_t targetCount, const AuroraMetalStereoTarget* panel) {
  using namespace aurora::metal_interop;
  return g_bridge && g_bridge->SetTargets(frameToken, targets, targetCount, panel);
}

bool aurora_metal_cancel_stereo_targets(uint64_t frameToken) {
  using namespace aurora::metal_interop;
  return g_bridge && g_bridge->CancelBeforeEncode(frameToken);
}

bool aurora_metal_forget_stereo_targets(void* const* ioSurfaces, uint32_t count) {
  using namespace aurora::metal_interop;
  if (!g_bridge) {
    return true;
  }
  return ioSurfaces != nullptr && g_bridge->ForgetTargets(ioSurfaces, count);
}

bool aurora_metal_disable_stereo_bridge() {
  using namespace aurora::metal_interop;
  if (!g_bridge) {
    return true;
  }
  aurora::stereo::set_sink(nullptr, nullptr, nullptr);
  g_bridge->CancelPending();
  if (!g_bridge->PrepareForDestruction()) {
    (void)g_bridge.release();
    return false;
  }
  g_bridge.reset();
  return true;
}

#else

bool aurora_metal_get_native_handles(AuroraMetalNativeHandles* handles) {
  if (handles != nullptr) {
    *handles = {};
  }
  return false;
}

bool aurora_metal_enable_stereo_bridge(AuroraMetalStereoSubmittedCallback, void*) { return false; }

bool aurora_metal_set_stereo_targets(uint64_t, const AuroraMetalStereoTarget*, uint32_t) { return false; }

bool aurora_metal_set_stereo_targets_with_panel(uint64_t, const AuroraMetalStereoTarget*, uint32_t,
                                                const AuroraMetalStereoTarget*) {
  return false;
}

bool aurora_metal_cancel_stereo_targets(uint64_t) { return false; }

bool aurora_metal_forget_stereo_targets(void* const*, uint32_t) { return true; }

bool aurora_metal_disable_stereo_bridge() { return true; }

#endif
