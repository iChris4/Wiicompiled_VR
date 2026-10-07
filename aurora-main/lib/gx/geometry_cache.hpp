#pragma once
// Static display lists kept on the GPU across frames (command_processor.cpp process_display_list).
//
// A display list that only draws (a BRRES shape's primitive list: the course, the karts, the
// characters) is resolved once into flat de-indexed records (deindex_plan's layout) in one persistent
// storage buffer (gfx/geometry_buffer.hpp), its triangle-list indices kept absolute into that buffer.
// Each frame a hit only stages those indices; as every block sits at a multiple of its stride, the
// surfaces of one material still merge into one draw. Keyed by the list's host address and size and
// the layout the records were resolved with (the format, the arrays and where each attribute lands);
// valid while the guest-RAM write tracking says neither the list nor the arrays it indexes moved
// (gfx::guest_write_generation), a moved list re-digested before it is dropped. The Metroid Prime
// port's design (its gx/geometry_cache), without its game-side geometry sets. GX thread only.

#include "../gfx/common.hpp"
#include "gx.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <span>
#include <vector>

namespace aurora::gx::geometry_cache {

struct Key {
  u64 list = 0;   // the display list's host address
  u32 nbytes = 0; // its size
  u64 layout = 0; // the resolved layout (command_processor.cpp deindex_plan_hash)
  bool operator==(const Key&) const = default;
  template <typename H>
  friend H AbslHashValue(H h, const Key& key) {
    return H::combine(std::move(h), key.list, key.nbytes, key.layout);
  }
};

struct Entry {
  u32 offset = 0; // in the geometry buffer, a multiple of stride
  u32 bytes = 0;  // of records, a multiple of 4
  u32 stride = 0;
  u32 vertexCount = 0;
  std::vector<u32> indices; // triangle list, absolute into the buffer
  // Validity: the write-tracking generations sampled before the bytes were read, and the list's digest
  // for a generation that moved (a neighbouring write bumps the same granule).
  u64 listGeneration = 0;
  u64 listDigest = 0;
  std::array<u64, GX_VA_TEX7 + 1> arrayGenerations{};
  // Frame interpolation identity of the list's first draw command, and the position-matrix slots all
  // its draws use, so a hit's draw matches what the uncached path would have recorded.
  HashType geometrySignature = 0;
  HashType topologySignature = 0;
  uint16_t firstMask = 0;
  uint16_t unionMask = 0;
  u16 firstVtxCount = 0;
  u8 firstPrim = 0;
};

// The entry for a key, or null.
Entry* find(const Key& key) noexcept;
// Stores a list's resolved records and their triangle-list indices (relative to the first record here,
// absolute in the entry), uploading the records through the frame's vertex staging. Null when the
// buffer has no room or the records are not whole 4-byte words: the list then draws the plain way.
Entry* insert(const Key& key, Entry&& entry, std::span<const u8> records, const std::vector<u32>& indices);
// Drops one entry; its block is reused only once the frames that may still draw it are done.
void erase(const Key& key) noexcept;
// A list whose bytes changed under the cache (the game rewrites some lists every frame): after a second change in
// quick succession the key is left to the plain path for a while rather than resolved and uploaded each frame.
void note_changed(const Key& key) noexcept;
bool is_volatile(const Key& key) noexcept;
// Asks for every entry to go (a race ends); applied at the next frame begin. Any thread.
void request_clear() noexcept;
// Frame begin on the GX thread: applies a requested clear, releases blocks retired long enough ago.
void begin_frame(u32 frameIndex) noexcept;
// The frame's uploads were encoded (end_batch_impl): the entries inserted this batch are final.
void uploads_encoded() noexcept;
// The frame's uploads were dropped (abort_frame): the entries inserted this batch never reached the GPU.
void uploads_dropped() noexcept;
// Drops everything at once (the device or the buffer goes).
void clear() noexcept;

struct Stats {
  u32 entries = 0;
  u64 residentBytes = 0;
  u64 capacity = 0;
};
Stats stats() noexcept;

// Per-window counters for the fpslog, reset by the reader.
extern std::atomic<uint32_t> g_hits;
extern std::atomic<uint32_t> g_misses;
extern std::atomic<uint32_t> g_revalidated;   // hits whose list generation moved but whose bytes had not
extern std::atomic<uint32_t> g_reresolved;    // entries dropped for changed bytes
extern std::atomic<uint32_t> g_inPlaceUnsupported; // lists the cache cannot take (register writes, lines...)
extern std::atomic<uint32_t> g_inPlaceUntracked;   // lists or arrays outside the write-tracked guest RAM
extern std::atomic<uint32_t> g_inPlaceFull;        // no block or no staging room this frame
extern std::atomic<uint32_t> g_inPlaceVolatile;    // lists the game keeps rewriting (is_volatile)
extern std::atomic<uint32_t> g_nativeDraws;        // lists drawn through native vertex input (gx/native_vertex.hpp)

} // namespace aurora::gx::geometry_cache
