// The geometry cache (lib/gx/geometry_cache.hpp) through the FIFO processor's display-list entry point
// (command_processor.cpp process_display_list), with the GPU side stubbed: a list that only draws is resolved
// once, drawn from the persistent buffer afterwards, and goes back to the plain path when the cache cannot vouch
// for its bytes.
#include "gx_test_common.hpp"

#include "gx/command_processor.hpp"
#include "gfx/geometry_buffer.hpp"
#include "gx/geometry_cache.hpp"
#include "gx/pipeline.hpp"

#include <array>
#include <utility>
#include <vector>

namespace aurora::gfx::testing {
void set_geometry_buffer_capacity_bytes(uint64_t bytes) noexcept;
const std::vector<std::pair<uint32_t, uint32_t>>& geometry_uploads() noexcept;
const std::vector<uint8_t>& last_geometry_upload_bytes() noexcept;
const std::vector<uint32_t>& last_pushed_indices32() noexcept;
void reset_geometry_records() noexcept;
} // namespace aurora::gfx::testing

namespace {
using namespace aurora;
using namespace aurora::gx;
namespace cache = aurora::gx::geometry_cache;

uint64_t s_generation = 1;
const void* s_trackedArray = nullptr;
uint64_t generation_hook(const void*, size_t) noexcept { return s_generation; }
uint64_t untracked_hook(const void*, size_t) noexcept { return aurora::kGuestWriteUntracked; }
// The array's granule moved while the list's did not.
uint64_t array_moved_hook(const void* data, size_t) noexcept { return data == s_trackedArray ? 7u : 1u; }
// The list's granule moved (a neighbouring write) while the array's did not.
const void* s_trackedList = nullptr;
uint64_t list_moved_hook(const void* data, size_t) noexcept { return data == s_trackedList ? 2u : 1u; }

// A triangle whose records are [position index, s, t]: elements 2, 0 and 1 of a 3-element S16 xyz array.
constexpr u8 kDraw = static_cast<u8>(GX_TRIANGLES) | static_cast<u8>(GX_VTXFMT0);

class GeometryCacheTest : public GXFifoTest {
protected:
  void SetUp() override {
    GXFifoTest::SetUp();
    gfx::testing::use_real_vertex_format_helpers(true);
    gfx::testing::use_draw_command_tracking(true);
    gfx::testing::reset_geometry_records();
    cache::clear();
    cache::begin_frame(0); // an earlier test may have moved the frame counter
    for (auto* counter : {&cache::g_hits, &cache::g_misses, &cache::g_revalidated, &cache::g_reresolved,
                          &cache::g_inPlaceUnsupported, &cache::g_inPlaceUntracked, &cache::g_inPlaceFull,
                          &cache::g_inPlaceVolatile, &cache::g_nativeDraws}) {
      counter->store(0);
    }
    gfx::set_geometry_buffer_capacity_mb(1);
    aurora::g_guestWriteGenerationHook = &generation_hook;
    s_generation = 1;
    auto& state = gxState();
    state.deindexVertices = true;
    state.projType = GX_PERSPECTIVE;
    for (auto& desc : state.vtxDesc) {
      desc = GX_NONE;
    }
    state.vtxDesc[GX_VA_POS] = GX_INDEX8;
    state.vtxDesc[GX_VA_TEX0] = GX_DIRECT;
    state.vtxFmts[GX_VTXFMT0].attrs[GX_VA_POS] = {GX_POS_XYZ, GX_S16, 0};
    state.vtxFmts[GX_VTXFMT0].attrs[GX_VA_TEX0] = {GX_TEX_ST, GX_U8, 0};
    state.arrays[GX_VA_POS].data = positions.data();
    s_trackedArray = positions.data();
    state.arrays[GX_VA_POS].size = positions.size();
    state.arrays[GX_VA_POS].stride = 6;
    state.arrays[GX_VA_POS].le = false;
    state.lastVtxFmt = GX_MAX_VTXFMT;
    state.stateDirty = true;
    state.pipelineStateGeneration = next_gx_state_epoch();
    list = {kDraw, 0, 3, 2, 0xAA, 0xBB, 0, 0xCC, 0xDD, 1, 0xEE, 0xFF};
  }
  void TearDown() override {
    aurora::g_guestWriteGenerationHook = nullptr;
    gfx::set_geometry_buffer_capacity_mb(0);
    cache::clear();
    gxState().deindexVertices = false;
    gfx::testing::use_real_vertex_format_helpers(false);
  }

  void call(const std::vector<u8>& bytes) {
    fifo::process_display_list(bytes.data(), static_cast<uint32_t>(bytes.size()));
  }
  const DrawData* last_draw() { return gfx::get_last_draw_command<DrawData>(); }

  std::array<u8, 18> positions{
      0x00, 0x01, 0x00, 0x02, 0x00, 0x03, // element 0
      0x10, 0x11, 0x10, 0x12, 0x10, 0x13, // element 1
      0x20, 0x21, 0x20, 0x22, 0x20, 0x23, // element 2
  };
  std::vector<u8> list;
};

TEST_F(GeometryCacheTest, ResolvesOnceThenDrawsFromTheBuffer) {
  call(list);
  ASSERT_EQ(gfx::testing::geometry_uploads().size(), 1u);
  // Three 12-byte records (position at 0, texture coordinate at 8), at the buffer's start.
  EXPECT_EQ(gfx::testing::geometry_uploads()[0], (std::pair<uint32_t, uint32_t>{0u, 36u}));
  EXPECT_EQ(gfx::testing::last_pushed_indices32(), (std::vector<uint32_t>{0, 1, 2}));
  EXPECT_TRUE(gfx::testing::last_pushed_vertices().empty()) << "the records went to the geometry buffer, not the frame";
  ASSERT_NE(last_draw(), nullptr);
  EXPECT_TRUE(last_draw()->cachedGeometry);
  EXPECT_EQ(last_draw()->vtxCount, 3u);
  EXPECT_EQ(last_draw()->indexCount, 3u);
  EXPECT_EQ(last_draw()->vertRange.offset, 0u);
  EXPECT_EQ(cache::g_misses.load(), 1u);
  EXPECT_EQ(cache::stats().entries, 1u);

  // The same list under the same state: a hit, staged as indices alone and folded into the draw before it.
  call(list);
  EXPECT_EQ(gfx::testing::geometry_uploads().size(), 1u);
  EXPECT_EQ(cache::g_hits.load(), 1u);
  EXPECT_EQ(gfx::g_mergedDrawCallCount, 1u);
  EXPECT_EQ(last_draw()->indexCount, 6u);
  EXPECT_EQ(last_draw()->vtxCount, 6u);
}

TEST_F(GeometryCacheTest, SecondListSitsAtAStrideMultipleWithAbsoluteIndices) {
  call(list);
  // Another list at another address, two records.
  std::vector<u8> other{kDraw, 0, 3, 0, 0x11, 0x22, 1, 0x33, 0x44, 2, 0x55, 0x66};
  gxState().stateDirty = true;
  call(other);
  ASSERT_EQ(gfx::testing::geometry_uploads().size(), 2u);
  EXPECT_EQ(gfx::testing::geometry_uploads()[1].first % 12u, 0u);
  const uint32_t base = gfx::testing::geometry_uploads()[1].first / 12u;
  EXPECT_EQ(gfx::testing::last_pushed_indices32(), (std::vector<uint32_t>{base, base + 1, base + 2}));
  EXPECT_EQ(cache::stats().entries, 2u);
}

TEST_F(GeometryCacheTest, MovedGenerationWithTheSameBytesRevalidates) {
  call(list);
  s_trackedList = list.data();
  aurora::g_guestWriteGenerationHook = &list_moved_hook;
  gxState().stateDirty = true;
  call(list);
  EXPECT_EQ(gfx::testing::geometry_uploads().size(), 1u);
  EXPECT_EQ(cache::g_revalidated.load(), 1u);
  EXPECT_EQ(cache::g_hits.load(), 1u);
}

TEST_F(GeometryCacheTest, ChangedBytesUnderAMovedGenerationReresolve) {
  call(list);
  list[3] = 1; // the first vertex now takes element 1
  s_generation = 2;
  gxState().stateDirty = true;
  call(list);
  EXPECT_EQ(gfx::testing::geometry_uploads().size(), 2u);
  EXPECT_EQ(cache::g_reresolved.load(), 1u);
  EXPECT_EQ(cache::g_misses.load(), 2u);
  EXPECT_EQ(cache::stats().entries, 1u);
}

TEST_F(GeometryCacheTest, ChangedArrayGenerationReresolves) {
  call(list);
  // The array's granule moved (its generation differs) while the list's did not: the records may be stale.
  aurora::g_guestWriteGenerationHook = &array_moved_hook;
  gxState().stateDirty = true;
  call(list);
  EXPECT_EQ(gfx::testing::geometry_uploads().size(), 2u);
  EXPECT_EQ(cache::g_reresolved.load(), 1u);
}

TEST_F(GeometryCacheTest, UntrackedMemoryDrawsInPlace) {
  aurora::g_guestWriteGenerationHook = &untracked_hook;
  call(list);
  EXPECT_TRUE(gfx::testing::geometry_uploads().empty());
  EXPECT_EQ(cache::g_inPlaceUntracked.load(), 1u);
  // The plain path still de-indexes into the frame's vertex staging.
  EXPECT_EQ(gfx::testing::last_pushed_vertices().size(), 36u);
  ASSERT_NE(last_draw(), nullptr);
  EXPECT_FALSE(last_draw()->cachedGeometry);
}

TEST_F(GeometryCacheTest, AListThatWritesRegistersDrawsInPlace) {
  // A CP register write (GX_LOAD_CP_REG 0x08, address, value) before the draw: not a shape's primitive list.
  std::vector<u8> withRegister{0x08, 0x50, 0, 0, 0, 0};
  withRegister.insert(withRegister.end(), list.begin(), list.end());
  call(withRegister);
  EXPECT_TRUE(gfx::testing::geometry_uploads().empty());
  EXPECT_EQ(cache::stats().entries, 0u);
}

TEST_F(GeometryCacheTest, ADrawOfAnotherFormatInTheListDrawsInPlace) {
  std::vector<u8> mixed = list;
  mixed.push_back(static_cast<u8>(GX_TRIANGLES) | static_cast<u8>(GX_VTXFMT1));
  mixed.push_back(0);
  mixed.push_back(0);
  call(mixed);
  EXPECT_TRUE(gfx::testing::geometry_uploads().empty());
  EXPECT_EQ(cache::g_inPlaceUnsupported.load(), 1u);
}

TEST_F(GeometryCacheTest, AFullBufferLeavesNewListsInPlace) {
  gfx::testing::set_geometry_buffer_capacity_bytes(48);
  call(list);
  ASSERT_EQ(gfx::testing::geometry_uploads().size(), 1u);
  std::vector<u8> other{kDraw, 0, 3, 0, 0x11, 0x22, 1, 0x33, 0x44, 2, 0x55, 0x66};
  gxState().stateDirty = true;
  call(other);
  EXPECT_EQ(gfx::testing::geometry_uploads().size(), 1u);
  EXPECT_EQ(cache::g_inPlaceFull.load(), 1u);
  EXPECT_EQ(gfx::testing::last_pushed_vertices().size(), 36u);
}

TEST_F(GeometryCacheTest, PlainDrawsNeverJoinACachedOne) {
  call(list);
  // The same draw command through the FIFO (no display list): the plain, de-indexed path.
  gxState().stateDirty = false;
  decode_fifo(list);
  EXPECT_EQ(gfx::g_mergedDrawCallCount, 0u);
  ASSERT_NE(last_draw(), nullptr);
  EXPECT_FALSE(last_draw()->cachedGeometry);
  // And a cached draw never joins a plain one.
  call(list);
  EXPECT_EQ(gfx::g_mergedDrawCallCount, 0u);
  EXPECT_TRUE(last_draw()->cachedGeometry);
}

TEST_F(GeometryCacheTest, ARequestedClearReleasesEveryEntryAtFrameBegin) {
  call(list);
  cache::request_clear();
  cache::begin_frame(1);
  EXPECT_EQ(cache::stats().entries, 0u);
  gxState().stateDirty = true;
  call(list);
  EXPECT_EQ(gfx::testing::geometry_uploads().size(), 2u);
}

TEST_F(GeometryCacheTest, DroppedUploadsForgetTheirEntries) {
  call(list);
  cache::uploads_dropped();
  EXPECT_EQ(cache::stats().entries, 0u);
  gxState().stateDirty = true;
  call(list);
  EXPECT_EQ(gfx::testing::geometry_uploads().size(), 2u);
  // The first block was reusable at once: the second insert took the same offset.
  EXPECT_EQ(gfx::testing::geometry_uploads()[1].first, 0u);
}

TEST_F(GeometryCacheTest, NativeVertexInputConvertsTheRecordsOnceAndMarksTheDraw) {
  gxState().nativeVertexInputRequested = true;
  call(list);
  ASSERT_EQ(gfx::testing::geometry_uploads().size(), 1u);
  ASSERT_NE(last_draw(), nullptr);
  EXPECT_TRUE(last_draw()->cachedGeometry);
  EXPECT_TRUE(last_draw()->nativeVertices);
  EXPECT_TRUE(gxState().nativeVertices);
  // The S16 position of element 2 arrives little-endian for the vertex-input stage; the U8 texture coordinate and
  // the padding are untouched.
  const auto& bytes = gfx::testing::last_geometry_upload_bytes();
  ASSERT_EQ(bytes.size(), 36u);
  EXPECT_EQ((std::vector<u8>(bytes.begin(), bytes.begin() + 12)),
            (std::vector<u8>{0x21, 0x20, 0x22, 0x20, 0x23, 0x20, 0, 0, 0xAA, 0xBB, 0, 0}));
  EXPECT_EQ(cache::g_nativeDraws.load(), 1u);

  // A plain draw after it goes back to the storage path and does not join it.
  decode_fifo(list);
  EXPECT_FALSE(gxState().nativeVertices);
  EXPECT_FALSE(last_draw()->nativeVertices);
  EXPECT_EQ(gfx::g_mergedDrawCallCount, 0u);

  // The same list without the request keeps a storage-path entry of its own (another key), big-endian as resolved.
  gxState().nativeVertexInputRequested = false;
  gxState().stateDirty = true;
  call(list);
  EXPECT_EQ(gfx::testing::geometry_uploads().size(), 2u);
  EXPECT_FALSE(last_draw()->nativeVertices);
  EXPECT_EQ(gfx::testing::last_geometry_upload_bytes()[0], 0x20);
  EXPECT_EQ(cache::stats().entries, 2u);
}

TEST_F(GeometryCacheTest, AListRewrittenEveryFrameIsLeftToThePlainPath) {
  call(list);
  // Changed bytes twice in quick succession: the second re-resolution marks the list volatile and draws it in place
  // from then on, until it has kept still for a while.
  for (u8 element : {u8{1}, u8{0}}) {
    list[3] = element;
    s_generation += 1;
    gxState().stateDirty = true;
    call(list);
  }
  EXPECT_EQ(cache::g_reresolved.load(), 2u);
  EXPECT_EQ(cache::g_inPlaceVolatile.load(), 1u);
  EXPECT_EQ(gfx::testing::geometry_uploads().size(), 2u) << "the second change was not resolved again";
  EXPECT_EQ(gfx::testing::last_pushed_vertices().size(), 36u);
  gxState().stateDirty = true;
  call(list);
  EXPECT_EQ(cache::g_inPlaceVolatile.load(), 2u);
  EXPECT_EQ(gfx::testing::geometry_uploads().size(), 2u);
  // Six hundred frames of quiet later it is cached again.
  cache::begin_frame(601);
  gxState().stateDirty = true;
  call(list);
  EXPECT_EQ(gfx::testing::geometry_uploads().size(), 3u);
}

TEST_F(GeometryCacheTest, AnOrthographicListStaysOnThePlainPath) {
  gxState().projType = GX_ORTHOGRAPHIC;
  call(list);
  EXPECT_TRUE(gfx::testing::geometry_uploads().empty());
  EXPECT_EQ(gfx::testing::last_pushed_vertices().size(), 36u);
}

} // namespace
