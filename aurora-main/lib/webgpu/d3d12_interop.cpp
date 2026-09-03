#include <aurora/d3d12_interop.h>

#include "../internal.hpp"
#include "../stereo.hpp"
#include "gpu.hpp"

#if defined(_WIN32) && defined(WEBGPU_DAWN) && defined(DAWN_ENABLE_BACKEND_D3D12)

#include <dawn/native/D3D12Backend.h>

#include <d3d12.h>
#include <dxgi1_4.h>
#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace aurora::d3d12_interop {
namespace {

using Microsoft::WRL::ComPtr;

Module Log("aurora::d3d12_interop");

constexpr uint64_t kUnfencedSubmission = (std::numeric_limits<uint64_t>::max)();

constexpr char kGetDeviceExport[] =
    "?GetD3D12Device@d3d12@native@dawn@@YA?AV?$ComPtr@UID3D12Device@@@WRL@Microsoft@@PEAUWGPUDeviceImpl@@@Z";
constexpr char kGetQueueExport[] =
    "?GetD3D12CommandQueue@d3d12@native@dawn@@YA?AV?$ComPtr@UID3D12CommandQueue@@@WRL@Microsoft@@PEAUWGPUDeviceImpl@@@Z";

struct NativeObjects {
  ComPtr<ID3D12Device> device;
  ComPtr<ID3D12CommandQueue> queue;
};

int64_t to_dxgi_format(wgpu::TextureFormat format) noexcept {
  switch (format) {
  case wgpu::TextureFormat::RGBA8Unorm:
    return DXGI_FORMAT_R8G8B8A8_UNORM;
  case wgpu::TextureFormat::RGBA8UnormSrgb:
    return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
  case wgpu::TextureFormat::BGRA8Unorm:
    return DXGI_FORMAT_B8G8R8A8_UNORM;
  case wgpu::TextureFormat::BGRA8UnormSrgb:
    return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
  case wgpu::TextureFormat::RGBA16Float:
    return DXGI_FORMAT_R16G16B16A16_FLOAT;
  default:
    return DXGI_FORMAT_UNKNOWN;
  }
}

bool same_copy_family(DXGI_FORMAT left, DXGI_FORMAT right) noexcept {
  const auto family = [](DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
      return 1;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
      return 2;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
      return 3;
    default:
      return 0;
    }
  };
  const int leftFamily = family(left);
  return leftFamily != 0 && leftFamily == family(right);
}

bool get_native_objects(NativeObjects& objects) noexcept {
  if (!webgpu::g_device || webgpu::g_backendType != wgpu::BackendType::D3D12) {
    return false;
  }

#if defined(__MINGW32__)
  // The distributed Dawn DLL is built by MSVC. LLVM-MinGW uses a different
  // C++ symbol spelling, but Win64's calling ABI is identical. Resolve the two
  // pinned native exports explicitly and keep all public interop C-compatible.
  static_assert(sizeof(ComPtr<ID3D12Device>) == sizeof(void*));
  HMODULE dawnModule = GetModuleHandleW(L"webgpu_dawn.dll");
  if (dawnModule == nullptr) {
    Log.error("webgpu_dawn.dll is not loaded; native D3D12 interop is unavailable");
    return false;
  }
  using GetDeviceFn = ComPtr<ID3D12Device> (*)(WGPUDevice);
  using GetQueueFn = ComPtr<ID3D12CommandQueue> (*)(WGPUDevice);
  const auto getDevice = reinterpret_cast<GetDeviceFn>(GetProcAddress(dawnModule, kGetDeviceExport));
  const auto getQueue = reinterpret_cast<GetQueueFn>(GetProcAddress(dawnModule, kGetQueueExport));
  if (getDevice == nullptr || getQueue == nullptr) {
    Log.error("Pinned Dawn native D3D12 exports are unavailable");
    return false;
  }
  objects.device = getDevice(webgpu::g_device.Get());
  objects.queue = getQueue(webgpu::g_device.Get());
#else
  objects.device = dawn::native::d3d12::GetD3D12Device(webgpu::g_device.Get());
  objects.queue = dawn::native::d3d12::GetD3D12CommandQueue(webgpu::g_device.Get());
#endif
  return objects.device != nullptr && objects.queue != nullptr;
}

// Dawn exposes this descriptor only through a native C++ type. Its ABI is a
// chained header followed by a ComPtr, so spell the wire layout locally and
// enter Dawn through the ordinary WebGPU C API instead of linking a C++ ctor.
struct SharedTextureMemoryD3D12ResourceWire {
  wgpu::ChainedStruct chain{};
  ComPtr<ID3D12Resource> resource;
};

struct SharedFenceDxgiHandleWire {
  wgpu::ChainedStruct chain{};
  void* handle = nullptr;
};

struct IntermediateEye {
  ComPtr<ID3D12Resource> resource;
  wgpu::SharedTextureMemory memory;
  wgpu::Texture texture;
  wgpu::TextureFormat webgpuFormat = wgpu::TextureFormat::Undefined;
  DXGI_FORMAT dxgiFormat = DXGI_FORMAT_UNKNOWN;
  uint32_t width = 0;
  uint32_t height = 0;
  bool initialized = false;
  bool accessBegun = false;
};

struct PendingTarget {
  ComPtr<ID3D12Resource> resource;
  uint32_t width = 0;
  uint32_t height = 0;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
};

struct InFlightCommand {
  uint64_t fenceValue = 0;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  // D3D12 command lists do not retain application resource references. Keep
  // both sides of every copy alive until this submission's fence completes;
  // an eye-size change may otherwise replace the bridge intermediate while
  // the GPU is still reading it.
  std::array<ComPtr<ID3D12Resource>, AURORA_D3D12_STEREO_MAX_TARGETS> sources;
  std::array<ComPtr<ID3D12Resource>, AURORA_D3D12_STEREO_MAX_TARGETS> destinations;
};

class StereoBridge final {
public:
  StereoBridge(NativeObjects objects, AuroraD3D12StereoSubmittedCallback callback,
               void* userdata) noexcept
      : m_device(std::move(objects.device)), m_queue(std::move(objects.queue)),
        m_callback(callback), m_userdata(userdata) {}

  bool Initialize() noexcept {
    if (!webgpu::g_device.HasFeature(wgpu::FeatureName::SharedTextureMemoryD3D12Resource)) {
      Log.error("Dawn device lacks SharedTextureMemoryD3D12Resource");
      return false;
    }
    if (FAILED(m_device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&m_fence)))) {
      Log.error("Could not create the D3D12 interop fence");
      return false;
    }

    if (webgpu::g_device.HasFeature(wgpu::FeatureName::SharedFenceDXGISharedHandle)) {
      HANDLE handle = nullptr;
      if (SUCCEEDED(m_device->CreateSharedHandle(m_fence.Get(), nullptr, GENERIC_ALL, nullptr,
                                                  &handle))) {
        SharedFenceDxgiHandleWire wire{};
        wire.chain.sType = wgpu::SType::SharedFenceDXGISharedHandleDescriptor;
        wire.handle = handle;
        const wgpu::SharedFenceDescriptor descriptor{
            .nextInChain = &wire.chain,
            .label = "Aurora D3D12 stereo interop fence",
        };
        m_webgpuFence = webgpu::g_device.ImportSharedFence(&descriptor);
        CloseHandle(handle);
      }
    }
    if (!m_webgpuFence) {
      // This is still ordered correctly because both APIs submit to the exact
      // same D3D12 queue. The explicit shared fence additionally describes the
      // dependency to Dawn when that optional feature is available.
      Log.warn("Dawn shared-fence import is unavailable; using same-queue ordering");
    }
    return true;
  }

  ~StereoBridge() {
    if (!m_gpuIdle) {
      (void)WaitForGpuLocked();
    }
  }

  bool PrepareForDestruction() noexcept {
    std::lock_guard lock(m_mutex);
    return WaitForGpuLocked();
  }

  bool SetTargets(uint64_t token, const AuroraD3D12StereoTarget* targets,
                  uint32_t targetCount) noexcept {
    if (token == 0 || targets == nullptr || targetCount == 0 ||
        targetCount > AURORA_D3D12_STEREO_MAX_TARGETS) {
      return false;
    }
    std::lock_guard lock(m_mutex);
    if (m_framePending || m_encoded) {
      return false;
    }
    for (uint32_t eye = 0; eye < targetCount; ++eye) {
      if (targets[eye].resource == nullptr || targets[eye].width == 0 ||
          targets[eye].height == 0 || targets[eye].dxgiFormat == DXGI_FORMAT_UNKNOWN) {
        return false;
      }
      auto* resource = static_cast<ID3D12Resource*>(targets[eye].resource);
      const D3D12_RESOURCE_DESC desc = resource->GetDesc();
      if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
          desc.Width < targets[eye].width || desc.Height < targets[eye].height ||
          desc.DepthOrArraySize != 1 || desc.MipLevels != 1 || desc.SampleDesc.Count != 1 ||
          !same_copy_family(desc.Format, static_cast<DXGI_FORMAT>(targets[eye].dxgiFormat))) {
        return false;
      }
      m_targets[eye] = {
          .resource = resource,
          .width = targets[eye].width,
          .height = targets[eye].height,
          .format = static_cast<DXGI_FORMAT>(targets[eye].dxgiFormat),
      };
    }
    for (uint32_t eye = targetCount; eye < m_targets.size(); ++eye) {
      m_targets[eye] = {};
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
    PublishAndClearFrameLocked(frame.frameToken, false);
    return false;
  }

  void Submitted(const stereo::SinkFrame& frame) noexcept {
    std::lock_guard lock(m_mutex);
    if (!m_framePending || !m_encoded || frame.frameToken != m_frameToken) {
      return;
    }
    const bool success = EndAccessLocked() && EnqueueNativeCopyLocked();
    PublishAndClearFrameLocked(frame.frameToken, success);
  }

  void CancelPending() noexcept {
    uint64_t token = 0;
    {
      std::lock_guard lock(m_mutex);
      if (!m_framePending) {
        return;
      }
      token = m_frameToken;
      if (m_encoded) {
        EndAccessLocked();
      }
      PublishAndClearFrameLocked(token, false);
    }
  }

  bool CancelBeforeEncode(uint64_t token) noexcept {
    // Never make the XR pacing thread wait behind an in-progress Encode. A
    // failed try-lock means Aurora may already own GPU-relevant work, so the
    // submitted callback remains authoritative.
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
  bool EnsureIntermediate(uint32_t eye, const stereo::EyeImage& source) noexcept {
    auto& intermediate = m_intermediates[eye];
    const DXGI_FORMAT sourceFormat = static_cast<DXGI_FORMAT>(to_dxgi_format(source.format));
    if (source.texture == nullptr || sourceFormat == DXGI_FORMAT_UNKNOWN ||
        source.size.width != m_targets[eye].width || source.size.height != m_targets[eye].height ||
        !same_copy_family(sourceFormat, m_targets[eye].format)) {
      Log.error("Stereo eye {} does not match its OpenXR D3D12 target", eye);
      return false;
    }
    if (intermediate.texture && intermediate.width == source.size.width &&
        intermediate.height == source.size.height && intermediate.webgpuFormat == source.format) {
      return true;
    }
    if (intermediate.accessBegun) {
      return false;
    }

    intermediate = {};
    const D3D12_HEAP_PROPERTIES heap{
        .Type = D3D12_HEAP_TYPE_DEFAULT,
        .CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
        .MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN,
        .CreationNodeMask = 1,
        .VisibleNodeMask = 1,
    };
    const D3D12_RESOURCE_DESC resourceDescriptor{
        .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
        .Alignment = 0,
        .Width = source.size.width,
        .Height = source.size.height,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .Format = sourceFormat,
        .SampleDesc = {1, 0},
        .Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
        .Flags = D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS,
    };
    if (FAILED(m_device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &resourceDescriptor, D3D12_RESOURCE_STATE_COMMON,
            nullptr, IID_PPV_ARGS(&intermediate.resource)))) {
      Log.error("Could not create D3D12 stereo intermediate for eye {}", eye);
      return false;
    }

    SharedTextureMemoryD3D12ResourceWire wire{};
    wire.chain.sType = wgpu::SType::SharedTextureMemoryD3D12ResourceDescriptor;
    wire.resource = intermediate.resource;
    const wgpu::SharedTextureMemoryDescriptor memoryDescriptor{
        .nextInChain = &wire.chain,
        .label = eye == 0 ? "OpenXR left eye intermediate" : "OpenXR right eye intermediate",
    };
    intermediate.memory = webgpu::g_device.ImportSharedTextureMemory(&memoryDescriptor);
    if (!intermediate.memory) {
      Log.error("Dawn rejected D3D12 stereo intermediate for eye {}", eye);
      intermediate = {};
      return false;
    }
    wgpu::SharedTextureMemoryProperties properties{};
    if (intermediate.memory.GetProperties(&properties) != wgpu::Status::Success ||
        properties.size.width != source.size.width || properties.size.height != source.size.height ||
        properties.format != source.format ||
        (properties.usage & wgpu::TextureUsage::CopyDst) == wgpu::TextureUsage::None) {
      Log.error("Dawn reported incompatible D3D12 shared-texture properties for eye {}", eye);
      intermediate = {};
      return false;
    }
    const wgpu::TextureDescriptor textureDescriptor{
        .label = eye == 0 ? "OpenXR left eye shared texture" : "OpenXR right eye shared texture",
        .usage = wgpu::TextureUsage::CopyDst,
        .dimension = wgpu::TextureDimension::e2D,
        .size = {source.size.width, source.size.height, 1},
        .format = source.format,
        .mipLevelCount = 1,
        .sampleCount = 1,
    };
    intermediate.texture = intermediate.memory.CreateTexture(&textureDescriptor);
    if (!intermediate.texture) {
      Log.error("Dawn could not wrap D3D12 stereo intermediate for eye {}", eye);
      intermediate = {};
      return false;
    }
    intermediate.webgpuFormat = source.format;
    intermediate.dxgiFormat = sourceFormat;
    intermediate.width = source.size.width;
    intermediate.height = source.size.height;
    return true;
  }

  bool EncodeLocked(wgpu::CommandEncoder& encoder, const stereo::SinkFrame& frame) noexcept {
    CollectCompletedCommandsLocked();
    for (uint32_t eye = 0; eye < m_targetCount; ++eye) {
      if (!EnsureIntermediate(eye, frame.eyes[eye])) {
        return false;
      }
    }
    for (uint32_t eye = 0; eye < m_targetCount; ++eye) {
      auto& intermediate = m_intermediates[eye];
      const std::array fences{m_webgpuFence};
      const std::array values{m_lastExternalFenceValue};
      wgpu::SharedTextureMemoryBeginAccessDescriptor begin{};
      begin.initialized = intermediate.initialized;
      if (m_webgpuFence && m_lastExternalFenceValue != 0) {
        begin.fenceCount = 1;
        begin.fences = fences.data();
        begin.signaledValueCount = 1;
        begin.signaledValues = values.data();
      }
      if (intermediate.memory.BeginAccess(intermediate.texture, &begin) != wgpu::Status::Success) {
        Log.error("Dawn BeginAccess failed for stereo eye {}", eye);
        for (uint32_t begunEye = 0; begunEye < eye; ++begunEye) {
          wgpu::SharedTextureMemoryEndAccessState end{};
          m_intermediates[begunEye].memory.EndAccess(m_intermediates[begunEye].texture, &end);
          m_intermediates[begunEye].initialized = end.initialized;
          m_intermediates[begunEye].accessBegun = false;
        }
        return false;
      }
      intermediate.accessBegun = true;
    }
    // Acquire every shared texture before recording any command that refers
    // to one. If a later BeginAccess fails, the rollback above can therefore
    // end the earlier accesses without leaving an unsubmitted copy that uses
    // a texture after its access interval.
    for (uint32_t eye = 0; eye < m_targetCount; ++eye) {
      const auto& intermediate = m_intermediates[eye];
      const wgpu::TexelCopyTextureInfo source{
          .texture = *frame.eyes[eye].texture,
          .mipLevel = 0,
          .origin = {},
          .aspect = wgpu::TextureAspect::All,
      };
      const wgpu::TexelCopyTextureInfo destination{
          .texture = intermediate.texture,
          .mipLevel = 0,
          .origin = {},
          .aspect = wgpu::TextureAspect::All,
      };
      const wgpu::Extent3D extent{intermediate.width, intermediate.height, 1};
      encoder.CopyTextureToTexture(&source, &destination, &extent);
    }
    return true;
  }

  bool EndAccessLocked() noexcept {
    bool success = true;
    for (uint32_t eye = 0; eye < m_targetCount; ++eye) {
      auto& intermediate = m_intermediates[eye];
      if (!intermediate.accessBegun) {
        success = false;
        continue;
      }
      wgpu::SharedTextureMemoryEndAccessState end{};
      if (intermediate.memory.EndAccess(intermediate.texture, &end) != wgpu::Status::Success) {
        Log.error("Dawn EndAccess failed for stereo eye {}", eye);
        success = false;
      } else {
        intermediate.initialized = end.initialized;
      }
      intermediate.accessBegun = false;
    }
    return success;
  }

  bool EnqueueNativeCopyLocked() noexcept {
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    std::array<ComPtr<ID3D12Resource>, AURORA_D3D12_STEREO_MAX_TARGETS> sources;
    std::array<ComPtr<ID3D12Resource>, AURORA_D3D12_STEREO_MAX_TARGETS> destinations;
    if (FAILED(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                 IID_PPV_ARGS(&allocator))) ||
        FAILED(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(),
                                           nullptr, IID_PPV_ARGS(&list)))) {
      Log.error("Could not create the D3D12 stereo copy command list");
      return false;
    }

    for (uint32_t eye = 0; eye < m_targetCount; ++eye) {
      const auto& source = m_intermediates[eye];
      const auto& destination = m_targets[eye];
      sources[eye] = source.resource;
      destinations[eye] = destination.resource;
      const std::array barriers{
          D3D12_RESOURCE_BARRIER{
              .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
              .Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE,
              .Transition = {source.resource.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                             D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE},
          },
          D3D12_RESOURCE_BARRIER{
              .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
              .Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE,
              .Transition = {destination.resource.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                             D3D12_RESOURCE_STATE_RENDER_TARGET,
                             D3D12_RESOURCE_STATE_COPY_DEST},
          },
      };
      list->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
      const D3D12_TEXTURE_COPY_LOCATION sourceLocation{
          .pResource = source.resource.Get(),
          .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
          .SubresourceIndex = 0,
      };
      const D3D12_TEXTURE_COPY_LOCATION destinationLocation{
          .pResource = destination.resource.Get(),
          .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
          .SubresourceIndex = 0,
      };
      const D3D12_BOX sourceBox{0, 0, 0, source.width, source.height, 1};
      list->CopyTextureRegion(&destinationLocation, 0, 0, 0, &sourceLocation, &sourceBox);
      const std::array restore{
          D3D12_RESOURCE_BARRIER{
              .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
              .Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE,
              .Transition = {source.resource.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                             D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON},
          },
          D3D12_RESOURCE_BARRIER{
              .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
              .Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE,
              .Transition = {destination.resource.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                             D3D12_RESOURCE_STATE_COPY_DEST,
                             D3D12_RESOURCE_STATE_RENDER_TARGET},
          },
      };
      list->ResourceBarrier(static_cast<UINT>(restore.size()), restore.data());
    }
    if (FAILED(list->Close())) {
      Log.error("Could not close the D3D12 stereo copy command list");
      return false;
    }
    ID3D12CommandList* lists[]{list.Get()};
    m_queue->ExecuteCommandLists(1, lists);
    m_gpuIdle = false;
    const uint64_t fenceValue = ++m_nextFenceValue;
    const HRESULT signalResult = m_queue->Signal(m_fence.Get(), fenceValue);
    m_commands.push_back({FAILED(signalResult) ? kUnfencedSubmission : fenceValue,
                          std::move(allocator), std::move(list),
                          std::move(sources), std::move(destinations)});
    if (FAILED(signalResult)) {
      // ExecuteCommandLists has already transferred work to the queue. Keep
      // every command/resource reference alive even though there is no usable
      // completion value; shutdown will retry with a queue-tail fence and leak
      // this small bridge on an unrecoverable device/queue failure.
      Log.error("Could not signal the D3D12 stereo copy fence");
      return false;
    }
    m_lastExternalFenceValue = fenceValue;
    return true;
  }

  void CollectCompletedCommandsLocked() noexcept {
    const uint64_t completed = m_fence ? m_fence->GetCompletedValue() : 0;
    std::erase_if(m_commands, [completed](const InFlightCommand& command) {
      return command.fenceValue != kUnfencedSubmission && command.fenceValue <= completed;
    });
  }

  void ClearFrameLocked() noexcept {
    for (auto& target : m_targets) {
      target = {};
    }
    m_frameToken = 0;
    m_targetCount = 0;
    m_framePending = false;
    m_encoded = false;
  }

  void PublishAndClearFrameLocked(uint64_t token, bool success) noexcept {
    // Publication is part of the bridge state transition: once another thread
    // can observe that this token is no longer cancellable, its submission
    // result must already be visible. The OpenXR callback only takes the
    // backend submission mutex; no backend path holds that mutex while entering
    // this bridge, so keeping m_mutex here preserves the lock order.
    Notify(token, success);
    ClearFrameLocked();
  }

  void Notify(uint64_t token, bool success) noexcept {
    if (m_callback != nullptr) {
      m_callback(token, success, m_userdata);
    }
  }

  bool WaitForGpuLocked() noexcept {
    if (m_gpuIdle) {
      return true;
    }
    if (!m_queue || !m_fence) {
      return m_commands.empty();
    }
    const uint64_t value = ++m_nextFenceValue;
    if (FAILED(m_queue->Signal(m_fence.Get(), value))) {
      Log.error("Could not signal a D3D12 queue-tail fence during stereo bridge shutdown");
      return false;
    }
    if (m_fence->GetCompletedValue() >= value) {
      m_commands.clear();
      m_gpuIdle = true;
      return true;
    }
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (event == nullptr) {
      Log.error("Could not create the D3D12 stereo shutdown fence event");
      return false;
    }
    bool complete = false;
    if (SUCCEEDED(m_fence->SetEventOnCompletion(value, event))) {
      complete = WaitForSingleObject(event, 5000) == WAIT_OBJECT_0;
    }
    CloseHandle(event);
    if (!complete) {
      Log.error("Timed out waiting for the D3D12 stereo queue to become idle");
      return false;
    }
    m_commands.clear();
    m_gpuIdle = true;
    return true;
  }

  std::mutex m_mutex;
  ComPtr<ID3D12Device> m_device;
  ComPtr<ID3D12CommandQueue> m_queue;
  ComPtr<ID3D12Fence> m_fence;
  wgpu::SharedFence m_webgpuFence;
  std::array<IntermediateEye, AURORA_D3D12_STEREO_MAX_TARGETS> m_intermediates{};
  std::array<PendingTarget, AURORA_D3D12_STEREO_MAX_TARGETS> m_targets{};
  std::vector<InFlightCommand> m_commands;
  AuroraD3D12StereoSubmittedCallback m_callback = nullptr;
  void* m_userdata = nullptr;
  uint64_t m_frameToken = 0;
  uint64_t m_nextFenceValue = 0;
  uint64_t m_lastExternalFenceValue = 0;
  uint32_t m_targetCount = 0;
  bool m_framePending = false;
  bool m_encoded = false;
  bool m_gpuIdle = true;
};

std::unique_ptr<StereoBridge> g_bridge;

bool sink_encode(wgpu::CommandEncoder& encoder, const stereo::SinkFrame& frame,
                 void* userdata) noexcept {
  return static_cast<StereoBridge*>(userdata)->Encode(encoder, frame);
}

void sink_submitted(const stereo::SinkFrame& frame, void* userdata) noexcept {
  static_cast<StereoBridge*>(userdata)->Submitted(frame);
}

} // namespace
} // namespace aurora::d3d12_interop

bool aurora_d3d12_get_native_handles(AuroraD3D12NativeHandles* handles) {
  if (handles == nullptr) {
    return false;
  }
  *handles = {};
  aurora::d3d12_interop::NativeObjects objects;
  if (!aurora::d3d12_interop::get_native_objects(objects)) {
    return false;
  }
  const int64_t colorFormat =
      aurora::d3d12_interop::to_dxgi_format(aurora::webgpu::g_graphicsConfig.surfaceConfiguration.format);
  if (colorFormat == DXGI_FORMAT_UNKNOWN) {
    return false;
  }
  const LUID luid = objects.device->GetAdapterLuid();
  *handles = {
      .device = objects.device.Get(),
      .queue = objects.queue.Get(),
      .colorDxgiFormat = colorFormat,
      .adapterLuidLow = luid.LowPart,
      .adapterLuidHigh = luid.HighPart,
  };
  return true;
}

bool aurora_d3d12_enable_stereo_bridge(AuroraD3D12StereoSubmittedCallback submitted,
                                       void* userdata) {
  using namespace aurora::d3d12_interop;
  if (g_bridge || submitted == nullptr) {
    return false;
  }
  NativeObjects objects;
  if (!get_native_objects(objects)) {
    return false;
  }
  auto bridge = std::make_unique<StereoBridge>(std::move(objects), submitted, userdata);
  if (!bridge->Initialize()) {
    return false;
  }
  aurora::stereo::set_sink(sink_encode, sink_submitted, bridge.get());
  g_bridge = std::move(bridge);
  return true;
}

bool aurora_d3d12_set_stereo_targets(uint64_t frameToken,
                                     const AuroraD3D12StereoTarget* targets,
                                     uint32_t targetCount) {
  using namespace aurora::d3d12_interop;
  return g_bridge && g_bridge->SetTargets(frameToken, targets, targetCount);
}

bool aurora_d3d12_cancel_stereo_targets(uint64_t frameToken) {
  using namespace aurora::d3d12_interop;
  return g_bridge && g_bridge->CancelBeforeEncode(frameToken);
}

bool aurora_d3d12_disable_stereo_bridge() {
  using namespace aurora::d3d12_interop;
  if (!g_bridge) {
    return true;
  }
  aurora::stereo::set_sink(nullptr, nullptr, nullptr);
  g_bridge->CancelPending();
  if (!g_bridge->PrepareForDestruction()) {
    // An already-enqueued command has no trustworthy completion marker. Keep
    // the bridge, queue, command lists and resource references alive for the
    // rest of the process rather than freeing memory the GPU may still touch.
    (void)g_bridge.release();
    return false;
  }
  g_bridge.reset();
  return true;
}

#else

bool aurora_d3d12_get_native_handles(AuroraD3D12NativeHandles* handles) {
  if (handles != nullptr) {
    *handles = {};
  }
  return false;
}

bool aurora_d3d12_enable_stereo_bridge(AuroraD3D12StereoSubmittedCallback, void*) {
  return false;
}

bool aurora_d3d12_set_stereo_targets(uint64_t, const AuroraD3D12StereoTarget*, uint32_t) {
  return false;
}

bool aurora_d3d12_cancel_stereo_targets(uint64_t) { return false; }

bool aurora_d3d12_disable_stereo_bridge() { return true; }

#endif
