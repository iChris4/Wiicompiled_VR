// Compiled inside VulkanBackend.cpp, using the pinned Dawn implementation (apply.py).
//
// Fragment density maps for Aurora's immersive eye render passes. Aurora uploads immutable RG8
// maps, binds each to the texture view its eye passes render through, and
// RecordBeginDynamicRenderPass chains the bound map into those passes. The maps are raw VkImages
// that Dawn's usage tracking never sees: they stay in FRAGMENT_DENSITY_MAP_OPTIMAL from their
// upload until the fenced deleter frees them.
#define AURORA_DAWN_FDM_IMPLEMENTATION
#include "aurora_dawn_fdm_abi.h"
#include "aurora_fdm.h"
#include "src/dawn/native/DynamicUploader.h"
#include "src/dawn/native/ResourceMemoryAllocation.h"
#include "src/dawn/native/vulkan/BufferVk.h"
#include "src/dawn/native/vulkan/CommandRecordingContextVk.h"
#include "src/dawn/native/vulkan/FencedDeleter.h"
#include "src/dawn/native/vulkan/QueueVk.h"
#include "src/dawn/native/vulkan/ResourceHeapVk.h"
#include "src/dawn/native/vulkan/ResourceMemoryAllocatorVk.h"
#include "src/dawn/native/vulkan/VulkanError.h"
#include "absl/container/flat_hash_map.h"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
namespace dawn::native::vulkan {
namespace {
std::atomic<bool> auroraFdmRequested{false};

struct AuroraFdmMap {
    VkImage image;
    VkImageView view;
    ResourceMemoryAllocation memory;
    uint32_t width = 0;
    uint32_t height = 0;
    ExecutionSerial readySerial = kMaxExecutionSerial;
};
struct AuroraFdmBinding {
    Ref<TextureViewBase> view;
    uint64_t map = 0;
};
struct AuroraFdmState {
    uint64_t nextMap = 1;
    absl::flat_hash_map<uint64_t, AuroraFdmMap> maps;
    absl::flat_hash_map<TextureViewBase*, AuroraFdmBinding> bindings;
};
// Per device. Also taken while a render pass is recorded, under the device lock Queue::Submit holds.
std::mutex auroraFdmMutex;
absl::flat_hash_map<Device*, std::unique_ptr<AuroraFdmState>> auroraFdmStates;

Device* AuroraFdmDevice(void* handle) {
    return ToBackend(FromAPI(static_cast<WGPUDevice>(handle)));
}

bool AuroraFdmEnabled(Device* device) {
    return device->GetDeviceInfo().HasExt(DeviceExt::FragmentDensityMap);
}

AuroraFdmState* AuroraFdmStateFor(Device* device, bool create) {
    auto it = auroraFdmStates.find(device);
    if (it != auroraFdmStates.end()) return it->second.get();
    if (!create) return nullptr;
    return auroraFdmStates.emplace(device, std::make_unique<AuroraFdmState>()).first->second.get();
}

bool AuroraFdmReady(Device* device, const AuroraFdmMap& map) {
    return device->GetQueue()->GetCompletedCommandSerial() >= map.readySerial;
}

void AuroraFdmRelease(Device* device, AuroraFdmMap& map) {
    if (map.view != VK_NULL_HANDLE) device->GetFencedDeleter()->DeleteWhenUnused(map.view);
    if (map.image != VK_NULL_HANDLE) device->GetFencedDeleter()->DeleteWhenUnused(map.image);
    device->GetResourceMemoryAllocator()->Deallocate(&map.memory);
    map.view = VkImageView{};
    map.image = VkImage{};
}

MaybeError AuroraFdmUpload(Device* device, uint32_t width, uint32_t height, const uint8_t* rg8, AuroraFdmMap& map) {
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_R8G8_UNORM;
    imageInfo.extent = {width, height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_FRAGMENT_DENSITY_MAP_BIT_EXT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    DAWN_TRY(CheckVkSuccess(device->fn.CreateImage(device->GetVkDevice(), &imageInfo, nullptr, &*map.image),
                            "CreateImage (fragment density map)"));

    VkMemoryRequirements requirements;
    device->fn.GetImageMemoryRequirements(device->GetVkDevice(), map.image, &requirements);
    DAWN_TRY_ASSIGN(map.memory,
                    device->GetResourceMemoryAllocator()->Allocate(requirements, MemoryKind::DeviceLocal));
    DAWN_TRY(CheckVkSuccess(device->fn.BindImageMemory(device->GetVkDevice(), map.image,
                                                       ToBackend(map.memory.GetResourceHeap())->GetMemory(),
                                                       map.memory.GetOffset()),
                            "BindImageMemory (fragment density map)"));

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = map.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8G8_UNORM;
    viewInfo.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                           VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    DAWN_TRY(CheckVkSuccess(device->fn.CreateImageView(device->GetVkDevice(), &viewInfo, nullptr, &*map.view),
                            "CreateImageView (fragment density map)"));

    Queue* queue = ToBackend(device->GetQueue());
    const uint64_t size = uint64_t(width) * height * 2;
    DAWN_TRY(device->GetDynamicUploader()->WithUploadReservation(
        size, device->GetOptimalBufferToTextureCopyOffsetAlignment(),
        [&](UploadReservation reservation) -> MaybeError {
            std::memcpy(reservation.mappedPointer.get(), rg8, size);
            CommandRecordingContext* context = queue->GetPendingRecordingContext();

            VkImageMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.srcAccessMask = 0;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = map.image;
            barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            device->fn.CmdPipelineBarrier(context->commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                          VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

            VkBufferImageCopy region{};
            region.bufferOffset = reservation.offsetInBuffer;
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {width, height, 1};
            device->fn.CmdCopyBufferToImage(context->commandBuffer, ToBackend(reservation.buffer.Get())->GetHandle(),
                                            map.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

            // A map without VK_IMAGE_VIEW_CREATE_FRAGMENT_DENSITY_MAP_DYNAMIC_BIT_EXT is read by the
            // host when a render pass using it is recorded, so the upload is also made visible to
            // host reads. The map is only used once this submission has completed.
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_FRAGMENT_DENSITY_MAP_READ_BIT_EXT | VK_ACCESS_HOST_READ_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_FRAGMENT_DENSITY_MAP_OPTIMAL_EXT;
            device->fn.CmdPipelineBarrier(context->commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                          VK_PIPELINE_STAGE_FRAGMENT_DENSITY_PROCESS_BIT_EXT |
                                              VK_PIPELINE_STAGE_HOST_BIT,
                                          0, 0, nullptr, 0, nullptr, 1, &barrier);
            return {};
        }));
    map.width = width;
    map.height = height;
    map.readySerial = queue->GetPendingCommandSerial();
    DAWN_TRY(queue->SubmitPendingCommands());
    return {};
}
}  // namespace

bool AuroraFdmWanted(const VulkanDeviceInfo& info, bool dynamicRendering) {
    return auroraFdmRequested.load() && dynamicRendering && info.HasExt(DeviceExt::FragmentDensityMap) &&
           info.fragmentDensityMapFeatures.fragmentDensityMap == VK_TRUE &&
           info.fragmentDensityMapFeatures.fragmentDensityMapNonSubsampledImages == VK_TRUE;
}

::VkImageView AuroraFdmViewFor(Device* device, TextureViewBase* view) {
    if (view == nullptr || !AuroraFdmEnabled(device)) return VK_NULL_HANDLE;
    std::lock_guard lock(auroraFdmMutex);
    AuroraFdmState* state = AuroraFdmStateFor(device, false);
    if (state == nullptr) return VK_NULL_HANDLE;
    auto binding = state->bindings.find(view);
    if (binding == state->bindings.end()) return VK_NULL_HANDLE;
    auto map = state->maps.find(binding->second.map);
    if (map == state->maps.end() || !AuroraFdmReady(device, map->second)) return VK_NULL_HANDLE;
    return map->second.view;
}

void AuroraFdmDestroy(Device* device) {
    std::unique_ptr<AuroraFdmState> state;
    {
        std::lock_guard lock(auroraFdmMutex);
        auto it = auroraFdmStates.find(device);
        if (it == auroraFdmStates.end()) return;
        state = std::move(it->second);
        auroraFdmStates.erase(it);
    }
    state->bindings.clear();
    for (auto& [id, map] : state->maps) AuroraFdmRelease(device, map);
}

extern "C" DAWN_NATIVE_EXPORT uint32_t AuroraDawnFdmVersion(void) { return AURORA_DAWN_FDM_ABI; }

extern "C" DAWN_NATIVE_EXPORT void AuroraDawnFdmRequest(int enable) { auroraFdmRequested.store(enable != 0); }

extern "C" DAWN_NATIVE_EXPORT int AuroraDawnFdmQuery(void* handle, AuroraDawnFdmCaps* caps) {
    Device* device = AuroraFdmDevice(handle);
    auto guard = device->GetGuard();
    const VulkanDeviceInfo& info = device->GetDeviceInfo();
    *caps = {};
    caps->enabled = AuroraFdmEnabled(device) ? 1 : 0;
    if (info.HasExt(DeviceExt::FragmentDensityMap)) {
        const auto& properties = info.fragmentDensityMapProperties;
        caps->minTexelWidth = properties.minFragmentDensityTexelSize.width;
        caps->minTexelHeight = properties.minFragmentDensityTexelSize.height;
        caps->maxTexelWidth = properties.maxFragmentDensityTexelSize.width;
        caps->maxTexelHeight = properties.maxFragmentDensityTexelSize.height;
    }
    return static_cast<int>(caps->enabled);
}

extern "C" DAWN_NATIVE_EXPORT uint64_t AuroraDawnFdmCreateMap(void* handle, uint32_t width, uint32_t height,
                                                               const uint8_t* rg8) {
    Device* device = AuroraFdmDevice(handle);
    auto guard = device->GetGuard();
    if (!AuroraFdmEnabled(device) || width == 0 || height == 0 || rg8 == nullptr) return 0;
    AuroraFdmMap map;
    if (device->ConsumedError(AuroraFdmUpload(device, width, height, rg8, map))) {
        AuroraFdmRelease(device, map);
        return 0;
    }
    std::lock_guard lock(auroraFdmMutex);
    AuroraFdmState* state = AuroraFdmStateFor(device, true);
    const uint64_t id = state->nextMap++;
    state->maps.emplace(id, std::move(map));
    return id;
}

extern "C" DAWN_NATIVE_EXPORT int AuroraDawnFdmMapReady(void* handle, uint64_t id) {
    Device* device = AuroraFdmDevice(handle);
    auto guard = device->GetGuard();
    std::lock_guard lock(auroraFdmMutex);
    AuroraFdmState* state = AuroraFdmStateFor(device, false);
    if (state == nullptr) return 0;
    auto map = state->maps.find(id);
    return map != state->maps.end() && AuroraFdmReady(device, map->second) ? 1 : 0;
}

extern "C" DAWN_NATIVE_EXPORT void AuroraDawnFdmReleaseMap(void* handle, uint64_t id) {
    Device* device = AuroraFdmDevice(handle);
    auto guard = device->GetGuard();
    std::lock_guard lock(auroraFdmMutex);
    AuroraFdmState* state = AuroraFdmStateFor(device, false);
    if (state == nullptr) return;
    auto map = state->maps.find(id);
    if (map == state->maps.end()) return;
    absl::erase_if(state->bindings, [id](const auto& binding) { return binding.second.map == id; });
    AuroraFdmRelease(device, map->second);
    state->maps.erase(map);
}

extern "C" DAWN_NATIVE_EXPORT int AuroraDawnFdmBind(void* handle, void* viewHandle, uint64_t id) {
    Device* device = AuroraFdmDevice(handle);
    auto guard = device->GetGuard();
    TextureViewBase* view = FromAPI(static_cast<WGPUTextureView>(viewHandle));
    if (view == nullptr) return 0;
    std::lock_guard lock(auroraFdmMutex);
    AuroraFdmState* state = AuroraFdmStateFor(device, id != 0);
    if (state == nullptr) return 1;
    if (id == 0) {
        state->bindings.erase(view);
        return 1;
    }
    auto map = state->maps.find(id);
    if (map == state->maps.end()) return 0;
    // VkRenderingInfo requires the map to reach the render area at the largest texel size.
    const Extent3D size = view->GetSingleSubresourceVirtualSize();
    const auto& properties = device->GetDeviceInfo().fragmentDensityMapProperties;
    const uint64_t maxTexelWidth = std::max<uint32_t>(properties.maxFragmentDensityTexelSize.width, 1);
    const uint64_t maxTexelHeight = std::max<uint32_t>(properties.maxFragmentDensityTexelSize.height, 1);
    if (map->second.width * maxTexelWidth < size.width || map->second.height * maxTexelHeight < size.height) {
        return 0;
    }
    state->bindings[view] = AuroraFdmBinding{Ref<TextureViewBase>(view), id};
    return 1;
}
}  // namespace dawn::native::vulkan
