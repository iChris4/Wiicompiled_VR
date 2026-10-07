#pragma once
// Native vertex fetch for the geometry cache's aligned, resident records (gx/geometry_cache.hpp): the attributes a
// record holds mapped onto WebGPU vertex formats, so a cached draw's vertex shader takes them through the GPU's
// vertex-input stage instead of decoding bytes from the storage buffer (on the Quest's Adreno the storage decode is
// the eye passes' limit: the Metroid Prime port measured 9.6 -> 5.1 ms of App GPU time from this). The record's
// stride and offsets stay as de-indexing laid them out, so the absolute indices and the merging of cached draws do
// not change; only the byte order of 2- and 4-byte numeric components is turned little-endian, once, when the cache
// entry is built (convert). The Metroid Prime port's design (its gx/native_vertex.hpp).
#include "gx.hpp"

#include <algorithm>
#include <array>
#include <span>

namespace aurora::gx::native_vertex {
constexpr size_t MaxAttributes = 16; // WebGPU's baseline vertex attribute limit.
struct Layout {
  std::array<wgpu::VertexAttribute, MaxAttributes> attributes{};
  size_t count = 0;
  bool valid = false;
};

// The vertex-input layout of `config`'s records, invalid when some attribute has no native format: NBT normals
// (nine components), packed colours other than RGBA8, lines and points, a stride or offset off its word.
inline Layout layout(const ShaderConfig& config) {
  Layout out;
  if (config.lineMode || config.vtxStride == 0 || config.vtxStride % 4 != 0 ||
      config.attrs[GX_VA_POS].attrType == GX_NONE) {
    return out;
  }
  for (u32 attr = GX_VA_PNMTXIDX; attr <= GX_VA_TEX7; ++attr) {
    const auto& a = config.attrs[attr];
    if (a.attrType == GX_NONE) {
      continue;
    }
    if (a.attrType != GX_DIRECT || out.count == MaxAttributes) {
      return out;
    }
    wgpu::VertexFormat format;
    u32 offset = a.offset;
    u32 bytes = 0;
    if (attr < GX_VA_POS) {
      // One-byte matrix index: fetch its aligned word and select the byte in the shader.
      offset &= ~3u;
      bytes = 4;
      format = wgpu::VertexFormat::Uint32;
    } else if (attr == GX_VA_CLR0 || attr == GX_VA_CLR1) {
      if (a.compType != GX_RGBA8) {
        return out;
      }
      format = wgpu::VertexFormat::Unorm8x4;
      bytes = 4;
    } else {
      if (a.cnt == 0 || a.cnt > 4 || offset % 4 != 0) {
        return out;
      }
      const bool pair = a.cnt <= 2;
      switch (a.compType) {
      case GX_F32: {
        constexpr std::array formats{wgpu::VertexFormat::Float32, wgpu::VertexFormat::Float32x2,
                                     wgpu::VertexFormat::Float32x3, wgpu::VertexFormat::Float32x4};
        format = formats[a.cnt - 1];
        bytes = a.cnt * 4;
        break;
      }
      case GX_U8:
        format = pair ? wgpu::VertexFormat::Uint8x2 : wgpu::VertexFormat::Uint8x4;
        bytes = pair ? 2 : 4;
        break;
      case GX_S8:
        format = pair ? wgpu::VertexFormat::Sint8x2 : wgpu::VertexFormat::Sint8x4;
        bytes = pair ? 2 : 4;
        break;
      case GX_U16:
        format = pair ? wgpu::VertexFormat::Uint16x2 : wgpu::VertexFormat::Uint16x4;
        bytes = pair ? 4 : 8;
        break;
      case GX_S16:
        format = pair ? wgpu::VertexFormat::Sint16x2 : wgpu::VertexFormat::Sint16x4;
        bytes = pair ? 4 : 8;
        break;
      default:
        return out;
      }
    }
    // Integer x3 uses an x4 fetch; the extra component is padding or the next attribute and is ignored.
    if (offset + bytes > config.vtxStride || offset % std::min(bytes, 4u) != 0) {
      return out;
    }
    out.attributes[out.count] = {.format = format, .offset = offset, .shaderLocation = static_cast<u32>(out.count)};
    ++out.count;
  }
  out.valid = true;
  return out;
}

// Turns the 2- and 4-byte numeric components of big-endian records little-endian in place. Colours (byte-wise) and
// 1-byte data are untouched, as are the components of an array that was little-endian already.
inline bool convert(const ShaderConfig& config, std::span<u8> records) {
  if (!layout(config).valid || records.size() % config.vtxStride != 0) {
    return false;
  }
  for (u32 attr = GX_VA_POS; attr <= GX_VA_TEX7; ++attr) {
    const auto& a = config.attrs[attr];
    if (a.attrType == GX_NONE || a.le || attr == GX_VA_CLR0 || attr == GX_VA_CLR1) {
      continue;
    }
    const u32 bytes = a.compType == GX_F32 ? 4 : (a.compType == GX_U16 || a.compType == GX_S16) ? 2 : 1;
    if (bytes == 1) {
      continue;
    }
    for (size_t vertex = 0; vertex < records.size(); vertex += config.vtxStride) {
      for (u32 component = 0; component < a.cnt; ++component) {
        auto* value = records.data() + vertex + a.offset + component * bytes;
        std::reverse(value, value + bytes);
      }
    }
  }
  return true;
}
} // namespace aurora::gx::native_vertex
