#include "geometry_cache.hpp"

#include "../gfx/geometry_buffer.hpp"

#include <absl/container/flat_hash_map.h>
#include <algorithm>

namespace aurora::gx::geometry_cache {

std::atomic<uint32_t> g_hits{0};
std::atomic<uint32_t> g_misses{0};
std::atomic<uint32_t> g_revalidated{0};
std::atomic<uint32_t> g_reresolved{0};
std::atomic<uint32_t> g_inPlaceUnsupported{0};
std::atomic<uint32_t> g_inPlaceUntracked{0};
std::atomic<uint32_t> g_inPlaceFull{0};
std::atomic<uint32_t> g_inPlaceVolatile{0};
std::atomic<uint32_t> g_nativeDraws{0};

namespace {
static Module Log("aurora::gx::geometry_cache");

struct FreeBlock {
  u32 offset;
  u32 size;
};
struct RetiredBlock {
  u32 offset;
  u32 size;
  u32 releaseFrame;
};
// Frames whose GPU work may still read a block: the staging slots rotate through this many frames
// (gfx/common.cpp FrameSlotCount), and the eye replays of a sealed frame run within them.
constexpr u32 kRetireFrames = 4;

absl::flat_hash_map<Key, Entry> g_entries;
// Lists whose bytes changed under the cache: how often, and when last (is_volatile).
struct Volatility {
  u32 strikes = 0;
  u32 lastFrame = 0;
};
absl::flat_hash_map<Key, Volatility> g_volatile;
// A second change within this many frames marks a list volatile for as long again after its last change.
constexpr u32 kVolatileFrames = 600;
std::vector<FreeBlock> g_free; // by offset, neighbours merged
std::vector<RetiredBlock> g_retired;
std::vector<Key> g_pendingKeys; // inserted since the last seal, their uploads not yet encoded
std::atomic<bool> g_clearRequested{false};
u32 g_frame = 0;
u64 g_capacity = 0;
u64 g_residentBytes = 0;
bool g_fullLogged = false;

void reset_blocks(u64 capacity) noexcept {
  g_entries.clear();
  g_volatile.clear();
  g_free.clear();
  g_retired.clear();
  g_pendingKeys.clear();
  g_capacity = capacity;
  g_residentBytes = 0;
  g_fullLogged = false;
  if (capacity != 0) {
    g_free.push_back({0, static_cast<u32>(std::min<u64>(capacity, UINT32_MAX))});
  }
}

// The buffer may appear or go between frames (its lazy creation, a device reset): the entries
// describe the old one.
void sync_capacity() noexcept {
  const u64 capacity = gfx::geometry_buffer_capacity();
  if (capacity != g_capacity) {
    reset_blocks(capacity);
  }
}

// First fit, at a multiple of `stride` so an absolute index is whole.
bool allocate(u32 bytes, u32 stride, u32& out) noexcept {
  for (size_t i = 0; i < g_free.size(); ++i) {
    const FreeBlock block = g_free[i];
    const u64 start = (static_cast<u64>(block.offset) + stride - 1) / stride * stride;
    const u64 end = start + bytes;
    const u64 blockEnd = static_cast<u64>(block.offset) + block.size;
    if (end > blockEnd) {
      continue;
    }
    out = static_cast<u32>(start);
    const u32 front = static_cast<u32>(start - block.offset);
    const u32 tail = static_cast<u32>(blockEnd - end);
    if (front != 0 && tail != 0) {
      g_free[i] = {block.offset, front};
      g_free.insert(g_free.begin() + static_cast<std::ptrdiff_t>(i) + 1, FreeBlock{static_cast<u32>(end), tail});
    } else if (front != 0) {
      g_free[i] = {block.offset, front};
    } else if (tail != 0) {
      g_free[i] = {static_cast<u32>(end), tail};
    } else {
      g_free.erase(g_free.begin() + static_cast<std::ptrdiff_t>(i));
    }
    return true;
  }
  return false;
}

void release(u32 offset, u32 size) noexcept {
  auto it = std::lower_bound(g_free.begin(), g_free.end(), offset,
                             [](const FreeBlock& block, u32 value) { return block.offset < value; });
  it = g_free.insert(it, FreeBlock{offset, size});
  if (it + 1 != g_free.end() && it->offset + it->size == (it + 1)->offset) {
    it->size += (it + 1)->size;
    g_free.erase(it + 1);
  }
  if (it != g_free.begin() && (it - 1)->offset + (it - 1)->size == it->offset) {
    (it - 1)->size += it->size;
    g_free.erase(it);
  }
}

void retire(const Entry& entry) noexcept {
  g_retired.push_back({entry.offset, entry.bytes, g_frame + kRetireFrames});
  g_residentBytes -= entry.bytes;
}
} // namespace

Entry* find(const Key& key) noexcept {
  sync_capacity();
  const auto it = g_entries.find(key);
  return it != g_entries.end() ? &it->second : nullptr;
}

Entry* insert(const Key& key, Entry&& entry, std::span<const u8> records, const std::vector<u32>& indices) {
  sync_capacity();
  const u32 bytes = static_cast<u32>(records.size());
  if (g_capacity == 0 || entry.stride == 0 || bytes == 0 || bytes % 4 != 0 || indices.empty()) {
    return nullptr;
  }
  if (const auto old = g_entries.find(key); old != g_entries.end()) {
    retire(old->second);
    g_entries.erase(old);
  }
  u32 offset = 0;
  if (!allocate(bytes, entry.stride, offset)) {
    if (!g_fullLogged) {
      g_fullLogged = true;
      Log.warn("Geometry cache full ({} MiB resident): the lists beyond it draw the plain way", g_residentBytes >> 20);
    }
    return nullptr;
  }
  if (!gfx::queue_geometry_upload(offset, records.data(), bytes)) {
    release(offset, bytes);
    return nullptr;
  }
  entry.offset = offset;
  entry.bytes = bytes;
  entry.vertexCount = bytes / entry.stride;
  const u32 base = offset / entry.stride;
  entry.indices.resize(indices.size());
  for (size_t i = 0; i < indices.size(); ++i) {
    entry.indices[i] = base + indices[i];
  }
  g_residentBytes += bytes;
  g_pendingKeys.push_back(key);
  const auto [it, inserted] = g_entries.emplace(key, std::move(entry));
  return &it->second;
}

void erase(const Key& key) noexcept {
  const auto it = g_entries.find(key);
  if (it == g_entries.end()) {
    return;
  }
  retire(it->second);
  g_entries.erase(it);
}

void note_changed(const Key& key) noexcept {
  auto& v = g_volatile[key];
  if (v.strikes != 0 && g_frame - v.lastFrame > kVolatileFrames) {
    v.strikes = 0;
  }
  ++v.strikes;
  v.lastFrame = g_frame;
}

bool is_volatile(const Key& key) noexcept {
  const auto it = g_volatile.find(key);
  if (it == g_volatile.end()) {
    return false;
  }
  if (g_frame - it->second.lastFrame > kVolatileFrames) {
    g_volatile.erase(it);
    return false;
  }
  return it->second.strikes >= 2;
}

void request_clear() noexcept { g_clearRequested.store(true, std::memory_order_release); }

void begin_frame(u32 frameIndex) noexcept {
  g_frame = frameIndex;
  if (g_clearRequested.exchange(false, std::memory_order_acq_rel)) {
    for (auto& [key, entry] : g_entries) {
      retire(entry);
    }
    g_entries.clear();
    g_pendingKeys.clear();
    // The addresses may hold another course's lists next: judge them afresh.
    g_volatile.clear();
    g_fullLogged = false;
  }
  // Release what no frame in flight can still read.
  for (size_t i = 0; i < g_retired.size();) {
    if (static_cast<int32_t>(g_frame - g_retired[i].releaseFrame) >= 0) {
      release(g_retired[i].offset, g_retired[i].size);
      g_retired[i] = g_retired.back();
      g_retired.pop_back();
    } else {
      ++i;
    }
  }
}

void uploads_encoded() noexcept { g_pendingKeys.clear(); }

void uploads_dropped() noexcept {
  // Their blocks hold nothing, so they can be reused at once.
  for (const Key& key : g_pendingKeys) {
    const auto it = g_entries.find(key);
    if (it == g_entries.end()) {
      continue;
    }
    g_residentBytes -= it->second.bytes;
    release(it->second.offset, it->second.bytes);
    g_entries.erase(it);
  }
  g_pendingKeys.clear();
}

void clear() noexcept { reset_blocks(0); }

Stats stats() noexcept {
  return {
      .entries = static_cast<u32>(g_entries.size()),
      .residentBytes = g_residentBytes,
      .capacity = g_capacity,
  };
}

} // namespace aurora::gx::geometry_cache
