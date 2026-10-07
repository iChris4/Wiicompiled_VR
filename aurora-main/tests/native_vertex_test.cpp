// Native vertex input for the geometry cache's records (lib/gx/native_vertex.hpp): which aligned layouts map onto
// WebGPU vertex formats, where each attribute is fetched, and the one-time byte-order conversion.
#include "gx/native_vertex.hpp"

#include <gtest/gtest.h>

#include <array>
#include <vector>

namespace {
using namespace aurora;
using namespace aurora::gx;

AttrConfig direct(u8 cnt, GXCompType comp, u8 offset, u8 frac = 0, bool le = false) {
  return AttrConfig{
      .attrType = GX_DIRECT,
      .cnt = cnt,
      .compType = static_cast<u8>(comp),
      .offset = offset,
      .stride = 0,
      .frac = frac,
      .le = le,
      .nrmIndexCount = 1,
  };
}

TEST(NativeVertexLayout, MapsAlignedRecordsOntoVertexFormats) {
  ShaderConfig config{};
  // A matrix index byte, then an S16 xyz position (fetched as four 16-bit values), then a U8 st texcoord.
  config.attrs[GX_VA_PNMTXIDX] = direct(1, GX_U8, 0);
  config.attrs[GX_VA_POS] = direct(3, GX_S16, 4, 2);
  config.attrs[GX_VA_TEX0] = direct(2, GX_U8, 12);
  config.vtxStride = 16;
  const auto layout = native_vertex::layout(config);
  ASSERT_TRUE(layout.valid);
  ASSERT_EQ(layout.count, 3u);
  EXPECT_EQ(layout.attributes[0].format, wgpu::VertexFormat::Uint32);
  EXPECT_EQ(layout.attributes[0].offset, 0u);
  EXPECT_EQ(layout.attributes[1].format, wgpu::VertexFormat::Sint16x4);
  EXPECT_EQ(layout.attributes[1].offset, 4u);
  EXPECT_EQ(layout.attributes[2].format, wgpu::VertexFormat::Uint8x2);
  EXPECT_EQ(layout.attributes[2].offset, 12u);
  for (size_t i = 0; i < layout.count; ++i) {
    EXPECT_EQ(layout.attributes[i].shaderLocation, i);
  }
}

TEST(NativeVertexLayout, RejectsWhatHasNoVertexFormat) {
  ShaderConfig config{};
  config.attrs[GX_VA_POS] = direct(3, GX_F32, 0);
  config.vtxStride = 12;
  EXPECT_TRUE(native_vertex::layout(config).valid);

  // Nine-component NBT normals.
  ShaderConfig nbt = config;
  nbt.attrs[GX_VA_NRM] = direct(9, GX_S16, 12);
  nbt.vtxStride = 32;
  EXPECT_FALSE(native_vertex::layout(nbt).valid);

  // A packed colour other than RGBA8.
  ShaderConfig packed = config;
  packed.attrs[GX_VA_CLR0] = direct(1, GX_RGB565, 12);
  packed.vtxStride = 16;
  EXPECT_FALSE(native_vertex::layout(packed).valid);
  packed.attrs[GX_VA_CLR0] = direct(1, GX_RGBA8, 12);
  ASSERT_TRUE(native_vertex::layout(packed).valid);
  EXPECT_EQ(native_vertex::layout(packed).attributes[1].format, wgpu::VertexFormat::Unorm8x4);

  // An S16 xyz at the end of a record too short for its four-value fetch.
  ShaderConfig tight = config;
  tight.attrs[GX_VA_POS] = direct(3, GX_S16, 0);
  tight.vtxStride = 8;
  EXPECT_TRUE(native_vertex::layout(tight).valid);
  tight.vtxStride = 6;
  EXPECT_FALSE(native_vertex::layout(tight).valid);

  // Lines, an indexed attribute left in the config, no position.
  ShaderConfig lines = config;
  lines.lineMode = 1;
  EXPECT_FALSE(native_vertex::layout(lines).valid);
  ShaderConfig indexed = config;
  indexed.attrs[GX_VA_POS].attrType = GX_INDEX16;
  EXPECT_FALSE(native_vertex::layout(indexed).valid);
  EXPECT_FALSE(native_vertex::layout(ShaderConfig{}).valid);
}

TEST(NativeVertexConvert, TurnsNumericComponentsLittleEndianOnce) {
  ShaderConfig config{};
  config.attrs[GX_VA_POS] = direct(3, GX_S16, 0);
  config.attrs[GX_VA_CLR0] = direct(1, GX_RGBA8, 8);
  config.attrs[GX_VA_TEX0] = direct(2, GX_F32, 12, 0, true); // a little-endian array: left alone
  config.attrs[GX_VA_TEX1] = direct(2, GX_U8, 20);
  config.vtxStride = 24;
  std::vector<u8> records{
      0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0, 0,       // position
      0xAA, 0xBB, 0xCC, 0xDD,                         // colour
      0x11, 0x12, 0x13, 0x14, 0x21, 0x22, 0x23, 0x24, // little-endian texcoord
      0x31, 0x32, 0, 0,                               // bytes
  };
  ASSERT_TRUE(native_vertex::convert(config, {records.data(), records.size()}));
  const std::vector<u8> expected{
      0x02, 0x01, 0x04, 0x03, 0x06, 0x05, 0, 0,       //
      0xAA, 0xBB, 0xCC, 0xDD,                         //
      0x11, 0x12, 0x13, 0x14, 0x21, 0x22, 0x23, 0x24, //
      0x31, 0x32, 0, 0,                               //
  };
  EXPECT_EQ(records, expected);
  // Records that are not whole, or a layout without a native form, convert nothing.
  std::vector<u8> partial(records.begin(), records.begin() + 20);
  EXPECT_FALSE(native_vertex::convert(config, {partial.data(), partial.size()}));
}
} // namespace
