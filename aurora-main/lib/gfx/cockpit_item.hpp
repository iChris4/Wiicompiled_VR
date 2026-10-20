// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "cockpit_item_data.hpp"
#include "stereo_multiview.hpp"
#include "texture_convert.hpp"
#include "../webgpu/gpu.hpp"

#include <array>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>

namespace aurora::gfx::cockpit_item {

inline std::mutex archiveMutex;
inline std::shared_ptr<const data::Archive> archive;
inline std::atomic<uint64_t> archiveRevision{0};

inline size_t mip_bytes(const data::Texture& texture,uint32_t mips) {
  size_t total=0;
  for(uint32_t level=0;level<mips;++level)
    total+=size_t(std::max(texture.width>>level,1))*std::max(texture.height>>level,1)*4;
  return total;
}
inline void set_archive(const void* bytes,uint32_t size) {
  { std::lock_guard lock(archiveMutex); if(archive) return; }
  auto parsed=std::make_shared<data::Archive>(data::parse_archive(bytes,size));
  for(auto& model:parsed->models) for(auto& texture:model.textures) {
    for(uint32_t mips:{texture.mips,1u}) {
      auto converted=convert_texture(texture.format,texture.width,texture.height,mips,
                                     ArrayRef<uint8_t>(texture.bytes));
      const size_t length=mip_bytes(texture,mips);
      if(converted.format!=wgpu::TextureFormat::RGBA8Unorm || converted.data.size()<length) continue;
      texture.rgba.assign(converted.data.data(),converted.data.data()+length);
      texture.mips=mips;
      break;
    }
  }
  std::lock_guard lock(archiveMutex);
  if(archive) return;
  archive=parsed->loaded?std::move(parsed):nullptr;
  ++archiveRevision;
}
inline bool has_model(uint8_t id) {
  const int index=data::model_index(id);
  if(index<0) return false;
  std::lock_guard lock(archiveMutex);
  return archive && archive->models[index].valid();
}

// Uniform images. WGSL: struct Material and struct Frame below.
struct GpuStage { uint32_t color[4],colorOp[4],alpha[4],alphaOp[4],misc[4];float konst[4],texGen[2][4]; };
struct GpuMaterial { GpuStage stages[4];float registers[4][4],materialColor[4];uint32_t info[4]; };
static_assert(sizeof(GpuMaterial)==608);
struct GpuFrame { float eyeFromModel[12],seatFromModel[12],projection[4],depth[4]; };
static_assert(sizeof(GpuFrame)==128);
// Static per-model vertices: billboards keep their origin in position (w=1)
// and their bone-local offset, which the vertex shader turns to face the eye.
struct GpuVertex { float position[4],offset[4],normal[4],color[4],uv[4]; };
static_assert(sizeof(GpuVertex)==80);

inline GpuMaterial gpu_material(const data::Material& material) {
  GpuMaterial out{};
  for(uint32_t i=0;i<material.stageCount;++i) {
    const auto& s=material.stages[i];
    auto& g=out.stages[i];
    const uint32_t c=s.color,a=s.alpha;
    g.color[0]=(c>>12)&15u;g.color[1]=(c>>8)&15u;g.color[2]=(c>>4)&15u;g.color[3]=c&15u;
    g.colorOp[0]=(c>>16)&3u;g.colorOp[1]=(c>>18)&1u;g.colorOp[2]=(c>>19)&1u;g.colorOp[3]=(c>>20)&3u;
    g.alpha[0]=(a>>13)&7u;g.alpha[1]=(a>>10)&7u;g.alpha[2]=(a>>7)&7u;g.alpha[3]=(a>>4)&7u;
    g.alphaOp[0]=(a>>16)&3u;g.alphaOp[1]=(a>>18)&1u;g.alphaOp[2]=(a>>19)&1u;g.alphaOp[3]=(a>>20)&3u;
    g.misc[0]=(c>>22)&3u;g.misc[1]=(a>>22)&3u;g.misc[2]=s.textured;g.misc[3]=s.rasterized;
    std::memcpy(g.konst,s.konst.data(),sizeof(g.konst));
    const auto& gen=material.texGens[s.texCoord];
    const auto& m=gen.matrix;
    const float row0[4]{m[0],m[1],m[2],gen.normal?1.f:0.f},row1[4]{m[3],m[4],m[5],float(gen.uvSet)};
    std::memcpy(g.texGen[0],row0,sizeof(row0));std::memcpy(g.texGen[1],row1,sizeof(row1));
  }
  for(int i=0;i<4;++i) std::memcpy(out.registers[i],material.registers[i].data(),sizeof(out.registers[i]));
  std::memcpy(out.materialColor,material.materialColor.data(),sizeof(out.materialColor));
  out.info[0]=material.stageCount;out.info[1]=material.alphaCompare;
  out.info[2]=material.colorControl;out.info[3]=material.alphaControl;
  return out;
}

struct GpuModel {
  wgpu::Buffer vertices;
  std::vector<wgpu::Texture> textures;
  std::vector<wgpu::Buffer> uniforms;
  std::vector<wgpu::BindGroup> materials;
};
inline std::array<GpuModel,15> gpuModels;
inline std::array<bool,15> gpuReady{};
inline std::shared_ptr<const data::Archive> gpuArchive;
inline uint64_t gpuRevision=0;
inline wgpu::Texture whiteTexture;
inline std::array<wgpu::Sampler,18> samplers;
inline wgpu::BindGroupLayout materialLayout,frameLayout;
inline wgpu::PipelineLayout pipelineLayout;
inline wgpu::ShaderModule shader;
// One frame uniform per eye: both eyes may be encoded before one submit.
inline std::array<wgpu::Buffer,2> frameBuffers;
inline std::array<wgpu::BindGroup,2> frameGroups;
struct PipelineKey {
  uint8_t cull=2,blendSrc=0,blendDst=0;
  bool blend=false,subtract=false,depthWrite=true;
  bool operator==(const PipelineKey&) const = default;
};
inline std::vector<std::pair<PipelineKey,wgpu::RenderPipeline>> pipelines;
// Both eyes at once in a multiview pass (gfx/stereo_multiview.hpp): one buffer with both eyes' frames,
// which the shader indexes by the view, and pipelines carrying the view mask.
inline wgpu::Buffer frameBufferMultiview;
inline wgpu::BindGroup frameGroupMultiview;
inline wgpu::BindGroupLayout frameLayoutMultiview;
inline wgpu::PipelineLayout pipelineLayoutMultiview;
inline wgpu::ShaderModule shaderMultiview;
inline std::vector<std::pair<PipelineKey,wgpu::RenderPipeline>> multiviewPipelines;
inline uint32_t pipelineSamples=0;
inline bool pipelineReversed=false;
inline wgpu::TextureFormat pipelineColor{},pipelineDepth{};

inline void shutdown() {
  gpuModels={};gpuReady={};gpuArchive.reset();gpuRevision=0;
  pipelines.clear();samplers={};whiteTexture=nullptr;shader=nullptr;
  frameBuffers={};frameGroups={};materialLayout=nullptr;frameLayout=nullptr;pipelineLayout=nullptr;
  multiviewPipelines.clear();frameBufferMultiview=nullptr;frameGroupMultiview=nullptr;frameLayoutMultiview=nullptr;
  pipelineLayoutMultiview=nullptr;shaderMultiview=nullptr;
  pipelineSamples=0;
}

inline void refresh_archive() {
  const uint64_t revision=archiveRevision.load(std::memory_order_acquire);
  if(revision==gpuRevision) return;
  std::lock_guard lock(archiveMutex);
  gpuArchive=archive;
  gpuModels={};gpuReady={};
  gpuRevision=revision;
}

// GX TEV, four stages at most (item materials use three). Konst selections are
// resolved on the CPU; each stage samples its own binding with its own texgen.
inline constexpr const char* tevShader=R"(
  struct Stage { color: vec4u, colorOp: vec4u, alpha: vec4u, alphaOp: vec4u, misc: vec4u, konst: vec4f,
                 texGen0: vec4f, texGen1: vec4f };
  struct Material { stages: array<Stage, 4>, registers: array<vec4f, 4>, materialColor: vec4f, info: vec4u };
  struct Frame { eye0: vec4f, eye1: vec4f, eye2: vec4f, seat0: vec4f, seat1: vec4f, seat2: vec4f,
                 projection: vec4f, depth: vec4f };
  @group(0) @binding(0) var<uniform> material: Material;
  @group(0) @binding(1) var sampler0: sampler;
  @group(0) @binding(2) var texture0: texture_2d<f32>;
  @group(0) @binding(3) var sampler1: sampler;
  @group(0) @binding(4) var texture1: texture_2d<f32>;
  @group(0) @binding(5) var sampler2: sampler;
  @group(0) @binding(6) var texture2: texture_2d<f32>;
  @group(0) @binding(7) var sampler3: sampler;
  @group(0) @binding(8) var texture3: texture_2d<f32>;
  @group(1) @binding(0) var<uniform> frame: Frame;
  struct Out { @builtin(position) position: vec4f, @location(0) color: vec4f,
               @location(1) uv01: vec4f, @location(2) uv23: vec4f };
  fn unit(v: vec3f) -> vec3f { return v / max(length(v), 1e-4); }
  // G3D texgen: a UV set or, for env maps, the view-space normal, then the SRT.
  fn texCoord(s: u32, normal: vec3f, uv: vec4f) -> vec2f {
    let g0 = material.stages[s].texGen0;
    let g1 = material.stages[s].texGen1;
    var base = select(uv.xy, uv.zw, g1.w > 0.5);
    if (g0.w > 0.5) { base = vec2f(0.5 * normal.x + 0.5, -0.5 * normal.y + 0.5); }
    return vec2f(dot(g0.xyz, vec3f(base, 1.0)), dot(g1.xyz, vec3f(base, 1.0)));
  }
  @vertex fn vs(@location(0) position: vec4f, @location(1) offset: vec4f, @location(2) normal: vec4f,
                @location(3) color: vec4f, @location(4) uv: vec4f) -> Out {
    let p4 = vec4f(position.xyz, 1.0);
    let p = vec3f(dot(frame.eye0, p4), dot(frame.eye1, p4), dot(frame.eye2, p4)) + offset.xyz * frame.depth.z;
    let billboard = position.w > 0.5;
    let n = normal.xyz;
    let eyeNormal = select(unit(vec3f(dot(frame.eye0.xyz, n), dot(frame.eye1.xyz, n), dot(frame.eye2.xyz, n))),
                           vec3f(0.0, 0.0, 1.0), billboard);
    var lit = 1.0;
    if (!billboard && (material.info.z & 2u) != 0u) {
      let seatNormal = unit(vec3f(dot(frame.seat0.xyz, n), dot(frame.seat1.xyz, n), dot(frame.seat2.xyz, n)));
      lit = 0.55 + 0.45 * abs(dot(seatNormal, vec3f(0.3, 0.8, 0.5)));
    }
    var o: Out;
    let z = frame.depth.x * p.z + frame.depth.y;
    o.position = vec4f(frame.projection.x * p.x + frame.projection.y * p.z,
                       frame.projection.z * p.y + frame.projection.w * p.z,
                       clamp(z, 0.0, max(-p.z, 0.0)), -p.z);
    o.color = vec4f(select(material.materialColor.rgb, color.rgb, (material.info.z & 1u) != 0u) * lit,
                    select(material.materialColor.a, color.a, (material.info.w & 1u) != 0u));
    o.uv01 = vec4f(texCoord(0u, eyeNormal, uv), texCoord(1u, eyeNormal, uv));
    o.uv23 = vec4f(texCoord(2u, eyeNormal, uv), texCoord(3u, eyeNormal, uv));
    return o;
  }
  fn colorIn(sel: u32, prev: vec4f, c0: vec4f, c1: vec4f, c2: vec4f, tex: vec4f, ras: vec4f, k: vec4f) -> vec3f {
    switch sel {
      case 0u: { return prev.rgb; } case 1u: { return vec3f(prev.a); }
      case 2u: { return c0.rgb; } case 3u: { return vec3f(c0.a); }
      case 4u: { return c1.rgb; } case 5u: { return vec3f(c1.a); }
      case 6u: { return c2.rgb; } case 7u: { return vec3f(c2.a); }
      case 8u: { return tex.rgb; } case 9u: { return vec3f(tex.a); }
      case 10u: { return ras.rgb; } case 11u: { return vec3f(ras.a); }
      case 12u: { return vec3f(1.0); } case 13u: { return vec3f(0.5); }
      case 14u: { return k.rgb; }
      default: { return vec3f(0.0); }
    }
  }
  fn alphaIn(sel: u32, prev: vec4f, c0: vec4f, c1: vec4f, c2: vec4f, tex: vec4f, ras: vec4f, k: vec4f) -> f32 {
    switch sel {
      case 0u: { return prev.a; } case 1u: { return c0.a; } case 2u: { return c1.a; } case 3u: { return c2.a; }
      case 4u: { return tex.a; } case 5u: { return ras.a; } case 6u: { return k.a; }
      default: { return 0.0; }
    }
  }
  fn tevBias(b: u32) -> f32 { if (b == 1u) { return 0.5; } if (b == 2u) { return -0.5; } return 0.0; }
  fn tevScale(s: u32) -> f32 { if (s == 1u) { return 2.0; } if (s == 2u) { return 4.0; } if (s == 3u) { return 0.5; } return 1.0; }
  fn alphaTest(f: u32, value: f32, reference: f32) -> bool {
    switch f {
      case 0u: { return false; } case 1u: { return value < reference; } case 2u: { return value == reference; }
      case 3u: { return value <= reference; } case 4u: { return value > reference; }
      case 5u: { return value != reference; } case 6u: { return value >= reference; }
      default: { return true; }
    }
  }
  @fragment fn fs(i: Out) -> @location(0) vec4f {
    var samples = array<vec4f, 4>(textureSample(texture0, sampler0, i.uv01.xy), textureSample(texture1, sampler1, i.uv01.zw),
                                  textureSample(texture2, sampler2, i.uv23.xy), textureSample(texture3, sampler3, i.uv23.zw));
    var prev = material.registers[0];
    var c0 = material.registers[1];
    var c1 = material.registers[2];
    var c2 = material.registers[3];
    var result = prev;
    for (var s = 0u; s < min(material.info.x, 4u); s++) {
      let st = material.stages[s];
      let tex = select(vec4f(1.0), samples[s], st.misc.z != 0u);
      let ras = select(vec4f(0.0), i.color, st.misc.w != 0u);
      let ca = colorIn(st.color.x, prev, c0, c1, c2, tex, ras, st.konst);
      let cb = colorIn(st.color.y, prev, c0, c1, c2, tex, ras, st.konst);
      let cc = colorIn(st.color.z, prev, c0, c1, c2, tex, ras, st.konst);
      let cd = colorIn(st.color.w, prev, c0, c1, c2, tex, ras, st.konst);
      var color = (cd + select(1.0, -1.0, st.colorOp.y != 0u) * mix(ca, cb, cc) + tevBias(st.colorOp.x)) * tevScale(st.colorOp.w);
      color = select(clamp(color, vec3f(-4.0), vec3f(4.0)), clamp(color, vec3f(0.0), vec3f(1.0)), st.colorOp.z != 0u);
      let aa = alphaIn(st.alpha.x, prev, c0, c1, c2, tex, ras, st.konst);
      let ab = alphaIn(st.alpha.y, prev, c0, c1, c2, tex, ras, st.konst);
      let ac = alphaIn(st.alpha.z, prev, c0, c1, c2, tex, ras, st.konst);
      let ad = alphaIn(st.alpha.w, prev, c0, c1, c2, tex, ras, st.konst);
      var alpha = (ad + select(1.0, -1.0, st.alphaOp.y != 0u) * mix(aa, ab, ac) + tevBias(st.alphaOp.x)) * tevScale(st.alphaOp.w);
      alpha = select(clamp(alpha, -4.0, 4.0), clamp(alpha, 0.0, 1.0), st.alphaOp.z != 0u);
      switch st.misc.x {
        case 1u: { c0 = vec4f(color, c0.a); } case 2u: { c1 = vec4f(color, c1.a); }
        case 3u: { c2 = vec4f(color, c2.a); } default: { prev = vec4f(color, prev.a); }
      }
      switch st.misc.y {
        case 1u: { c0.a = alpha; } case 2u: { c1.a = alpha; }
        case 3u: { c2.a = alpha; } default: { prev.a = alpha; }
      }
      result = vec4f(color, alpha);
    }
    result = clamp(result, vec4f(0.0), vec4f(1.0));
    let word = material.info.y;
    let a8 = round(result.a * 255.0);
    let pass0 = alphaTest((word >> 16u) & 7u, a8, f32(word & 255u));
    let pass1 = alphaTest((word >> 19u) & 7u, a8, f32((word >> 8u) & 255u));
    let logic = (word >> 22u) & 3u;
    var passed = pass0 && pass1;
    if (logic == 1u) { passed = pass0 || pass1; } else if (logic == 2u) { passed = pass0 != pass1; }
    else if (logic == 3u) { passed = pass0 == pass1; }
    if (!passed) { discard; }
    return result;
  }
)";

inline const wgpu::Sampler& sampler(uint8_t wrapS,uint8_t wrapT,bool mipmapped) {
  auto& slot=samplers[(wrapS*3+wrapT)*2+mipmapped];
  if(!slot) {
    constexpr wgpu::AddressMode modes[3]{wgpu::AddressMode::ClampToEdge,wgpu::AddressMode::Repeat,
                                         wgpu::AddressMode::MirrorRepeat};
    const wgpu::SamplerDescriptor desc{.label="Cockpit item sampler",
      .addressModeU=modes[wrapS],.addressModeV=modes[wrapT],
      .magFilter=wgpu::FilterMode::Linear,.minFilter=wgpu::FilterMode::Linear,
      .mipmapFilter=mipmapped?wgpu::MipmapFilterMode::Linear:wgpu::MipmapFilterMode::Nearest};
    slot=webgpu::g_device.CreateSampler(&desc);
  }
  return slot;
}

inline void prepare_layout() {
  using namespace webgpu;
  if(materialLayout) return;
  std::array<wgpu::BindGroupLayoutEntry,9> entries{};
  entries[0]={.binding=0,.visibility=wgpu::ShaderStage::Vertex|wgpu::ShaderStage::Fragment,
              .buffer=wgpu::BufferBindingLayout{.type=wgpu::BufferBindingType::Uniform,.minBindingSize=sizeof(GpuMaterial)}};
  for(uint32_t i=0;i<4;++i) {
    entries[1+i*2]={.binding=1+i*2,.visibility=wgpu::ShaderStage::Fragment,
      .sampler=wgpu::SamplerBindingLayout{.type=wgpu::SamplerBindingType::Filtering}};
    entries[2+i*2]={.binding=2+i*2,.visibility=wgpu::ShaderStage::Fragment,
      .texture=wgpu::TextureBindingLayout{.sampleType=wgpu::TextureSampleType::Float,
                                          .viewDimension=wgpu::TextureViewDimension::e2D}};
  }
  const wgpu::BindGroupLayoutDescriptor materialDesc{.entryCount=entries.size(),.entries=entries.data()};
  materialLayout=g_device.CreateBindGroupLayout(&materialDesc);
  const wgpu::BindGroupLayoutEntry frameEntry{.binding=0,.visibility=wgpu::ShaderStage::Vertex,
    .buffer=wgpu::BufferBindingLayout{.type=wgpu::BufferBindingType::Uniform,.minBindingSize=sizeof(GpuFrame)}};
  const wgpu::BindGroupLayoutDescriptor frameDesc{.entryCount=1,.entries=&frameEntry};
  frameLayout=g_device.CreateBindGroupLayout(&frameDesc);
  const std::array layouts{materialLayout,frameLayout};
  const wgpu::PipelineLayoutDescriptor layoutDesc{.bindGroupLayoutCount=layouts.size(),.bindGroupLayouts=layouts.data()};
  pipelineLayout=g_device.CreatePipelineLayout(&layoutDesc);
  wgpu::ShaderSourceWGSL source{};
  source.code=tevShader;
  wgpu::ShaderModuleDescriptor md{};md.nextInChain=&source;md.label="Cockpit item TEV";
  shader=g_device.CreateShaderModule(&md);
  for(uint32_t eye=0;eye<2;++eye) {
    const wgpu::BufferDescriptor bufferDesc{.label="Cockpit item frame",
      .usage=wgpu::BufferUsage::Uniform|wgpu::BufferUsage::CopyDst,.size=sizeof(GpuFrame)};
    frameBuffers[eye]=g_device.CreateBuffer(&bufferDesc);
    const wgpu::BindGroupEntry entry{.binding=0,.buffer=frameBuffers[eye],.size=sizeof(GpuFrame)};
    const wgpu::BindGroupDescriptor group{.layout=frameLayout,.entryCount=1,.entries=&entry};
    frameGroups[eye]=g_device.CreateBindGroup(&group);
  }
#if defined(WGPU_DAWN_RENDER_PIPELINE_MULTIVIEW_INIT)
  if(stereo_multiview::supported()) {
    // The multiview variant: both eyes' frames in one uniform, picked by @builtin(view_index).
    const wgpu::BindGroupLayoutEntry framesEntry{.binding=0,.visibility=wgpu::ShaderStage::Vertex,
      .buffer=wgpu::BufferBindingLayout{.type=wgpu::BufferBindingType::Uniform,.minBindingSize=2*sizeof(GpuFrame)}};
    const wgpu::BindGroupLayoutDescriptor framesDesc{.entryCount=1,.entries=&framesEntry};
    frameLayoutMultiview=g_device.CreateBindGroupLayout(&framesDesc);
    const std::array layoutsMultiview{materialLayout,frameLayoutMultiview};
    const wgpu::PipelineLayoutDescriptor layoutDescMultiview{.bindGroupLayoutCount=layoutsMultiview.size(),
      .bindGroupLayouts=layoutsMultiview.data()};
    pipelineLayoutMultiview=g_device.CreatePipelineLayout(&layoutDescMultiview);
    std::string code=tevShader;
    const auto replace=[&](const char* from,const char* to) {
      const size_t at=code.find(from);
      if(at!=std::string::npos) code.replace(at,std::strlen(from),to);
    };
    replace("@group(1) @binding(0) var<uniform> frame: Frame;","@group(1) @binding(0) var<uniform> frames: array<Frame, 2>;");
    replace("@vertex fn vs(@location(0) position: vec4f,","@vertex fn vs(@builtin(view_index) view: u32, @location(0) position: vec4f,");
    replace("    let p4 = vec4f(position.xyz, 1.0);","    let frame = &frames[view];\n    let p4 = vec4f(position.xyz, 1.0);");
    code="enable chromium_experimental_multiview;\n"+code;
    wgpu::ShaderSourceWGSL sourceMultiview{};
    sourceMultiview.code=code.c_str();
    wgpu::ShaderModuleDescriptor mdMultiview{};mdMultiview.nextInChain=&sourceMultiview;mdMultiview.label="Cockpit item TEV (multiview)";
    shaderMultiview=g_device.CreateShaderModule(&mdMultiview);
    const wgpu::BufferDescriptor framesBufferDesc{.label="Cockpit item frames (multiview)",
      .usage=wgpu::BufferUsage::Uniform|wgpu::BufferUsage::CopyDst,.size=2*sizeof(GpuFrame)};
    frameBufferMultiview=g_device.CreateBuffer(&framesBufferDesc);
    const wgpu::BindGroupEntry framesGroupEntry{.binding=0,.buffer=frameBufferMultiview,.size=2*sizeof(GpuFrame)};
    const wgpu::BindGroupDescriptor framesGroup{.layout=frameLayoutMultiview,.entryCount=1,.entries=&framesGroupEntry};
    frameGroupMultiview=g_device.CreateBindGroup(&framesGroup);
  }
#endif
  const wgpu::TextureDescriptor desc{.label="Cockpit item white",
    .usage=wgpu::TextureUsage::TextureBinding|wgpu::TextureUsage::CopyDst,
    .dimension=wgpu::TextureDimension::e2D,.size={1,1,1},.format=wgpu::TextureFormat::RGBA8Unorm,
    .mipLevelCount=1,.sampleCount=1};
  whiteTexture=g_device.CreateTexture(&desc);
  const uint8_t white[4]{255,255,255,255};
  const wgpu::TexelCopyTextureInfo destination{.texture=whiteTexture};
  const wgpu::TexelCopyBufferLayout layout{.bytesPerRow=4,.rowsPerImage=1};
  const wgpu::Extent3D extent{1,1,1};
  g_queue.WriteTexture(&destination,white,4,&layout,&extent);
}

inline void prepare_model(size_t index) {
  using namespace webgpu;
  const auto& model=gpuArchive->models[index];
  auto& gpu=gpuModels[index];
  std::vector<GpuVertex> vertices;
  for(const auto& part:model.parts) for(const auto& v:part.vertices) {
    const data::V3 at=part.billboard?part.origin:v.position,offset=part.billboard?v.position:data::V3{};
    vertices.push_back({{at.x,at.y,at.z,part.billboard?1.f:0.f},{offset.x,offset.y,offset.z,0},
                        {v.normal.x,v.normal.y,v.normal.z,0},{v.color[0],v.color[1],v.color[2],v.color[3]},
                        {v.uv[0].x,v.uv[0].y,v.uv[1].x,v.uv[1].y}});
  }
  const wgpu::BufferDescriptor vertexDesc{.label="Cockpit item vertices",
    .usage=wgpu::BufferUsage::Vertex|wgpu::BufferUsage::CopyDst,.size=vertices.size()*sizeof(GpuVertex)};
  gpu.vertices=g_device.CreateBuffer(&vertexDesc);
  g_queue.WriteBuffer(gpu.vertices,0,vertices.data(),vertices.size()*sizeof(GpuVertex));
  for(const auto& texture:model.textures) {
    const bool usable=!texture.rgba.empty() && texture.rgba.size()>=mip_bytes(texture,texture.mips);
    if(!usable) { gpu.textures.push_back(nullptr);continue; }
    const wgpu::TextureDescriptor desc{.label="Cockpit item texture",
      .usage=wgpu::TextureUsage::TextureBinding|wgpu::TextureUsage::CopyDst,
      .dimension=wgpu::TextureDimension::e2D,.size={texture.width,texture.height,1},
      .format=wgpu::TextureFormat::RGBA8Unorm,.mipLevelCount=texture.mips,.sampleCount=1};
    auto gpuTexture=g_device.CreateTexture(&desc);
    size_t offset=0;
    for(uint32_t level=0;level<texture.mips;++level) {
      const uint32_t w=std::max(texture.width>>level,1),h=std::max(texture.height>>level,1);
      const wgpu::TexelCopyTextureInfo destination{.texture=gpuTexture,.mipLevel=level};
      const wgpu::TexelCopyBufferLayout layout{.bytesPerRow=w*4,.rowsPerImage=h};
      const wgpu::Extent3D extent{w,h,1};
      g_queue.WriteTexture(&destination,texture.rgba.data()+offset,size_t(w)*h*4,&layout,&extent);
      offset+=size_t(w)*h*4;
    }
    gpu.textures.push_back(std::move(gpuTexture));
  }
  for(const auto& material:model.materials) {
    const auto uniform=gpu_material(material);
    const wgpu::BufferDescriptor bufferDesc{.label="Cockpit item material",
      .usage=wgpu::BufferUsage::Uniform|wgpu::BufferUsage::CopyDst,.size=sizeof(GpuMaterial)};
    auto buffer=g_device.CreateBuffer(&bufferDesc);
    g_queue.WriteBuffer(buffer,0,&uniform,sizeof(uniform));
    std::array<wgpu::BindGroupEntry,9> entries{};
    entries[0]={.binding=0,.buffer=buffer,.size=sizeof(GpuMaterial)};
    for(uint32_t i=0;i<4;++i) {
      const auto& stage=material.stages[i];
      const data::Map* map=i<material.stageCount && stage.textured?&material.maps[stage.texMap]:nullptr;
      const wgpu::Texture* texture=map && gpu.textures[map->texture]?&gpu.textures[map->texture]:nullptr;
      const bool mipmapped=texture && model.textures[map->texture].mips>1;
      entries[1+i*2]={.binding=1+i*2,.sampler=texture?sampler(map->wrapS,map->wrapT,mipmapped):sampler(1,1,false)};
      entries[2+i*2]={.binding=2+i*2,.textureView=(texture?*texture:whiteTexture).CreateView()};
    }
    const wgpu::BindGroupDescriptor group{.layout=materialLayout,.entryCount=entries.size(),.entries=entries.data()};
    gpu.materials.push_back(g_device.CreateBindGroup(&group));
    gpu.uniforms.push_back(std::move(buffer));
  }
  gpuReady[index]=true;
}

inline wgpu::BlendFactor blend_factor(uint8_t factor,bool source) {
  switch(factor) {
  case 0: return wgpu::BlendFactor::Zero;
  case 1: return wgpu::BlendFactor::One;
  case 2: return source?wgpu::BlendFactor::Dst:wgpu::BlendFactor::Src;
  case 3: return source?wgpu::BlendFactor::OneMinusDst:wgpu::BlendFactor::OneMinusSrc;
  case 4: return wgpu::BlendFactor::SrcAlpha;
  case 5: return wgpu::BlendFactor::OneMinusSrcAlpha;
  case 6: return wgpu::BlendFactor::One;   // The eye target's alpha is not the EFB's.
  default: return wgpu::BlendFactor::Zero;
  }
}
inline const wgpu::RenderPipeline& pipeline(const PipelineKey& key,const StereoReplayFrame& frame,uint32_t eye,bool reversed,
                                            bool multiview=false) {
  using namespace webgpu;
  const auto& target=frame.eyes[eye].target;
  const auto format=g_graphicsConfig.surfaceConfiguration.format;
  if(pipelineSamples!=target.msaaSamples || pipelineColor!=format ||
     pipelineDepth!=target.depthFormat || pipelineReversed!=reversed) {
    pipelines.clear();multiviewPipelines.clear();
    pipelineSamples=target.msaaSamples;pipelineColor=format;pipelineDepth=target.depthFormat;pipelineReversed=reversed;
  }
  auto& cache=multiview?multiviewPipelines:pipelines;
  const auto& module=multiview?shaderMultiview:shader;
  for(const auto& [cached,value]:cache) if(cached==key) return value;
  const wgpu::VertexAttribute attrs[]{
    {.format=wgpu::VertexFormat::Float32x4,.offset=0,.shaderLocation=0},
    {.format=wgpu::VertexFormat::Float32x4,.offset=16,.shaderLocation=1},
    {.format=wgpu::VertexFormat::Float32x4,.offset=32,.shaderLocation=2},
    {.format=wgpu::VertexFormat::Float32x4,.offset=48,.shaderLocation=3},
    {.format=wgpu::VertexFormat::Float32x4,.offset=64,.shaderLocation=4},
  };
  const wgpu::VertexBufferLayout vertices{.arrayStride=sizeof(GpuVertex),.attributeCount=5,.attributes=attrs};
  const wgpu::BlendState blend{
    .color=key.subtract?wgpu::BlendComponent{.operation=wgpu::BlendOperation::ReverseSubtract,
                                             .srcFactor=wgpu::BlendFactor::One,.dstFactor=wgpu::BlendFactor::One}
                       :wgpu::BlendComponent{.operation=wgpu::BlendOperation::Add,
                                             .srcFactor=blend_factor(key.blendSrc,true),
                                             .dstFactor=blend_factor(key.blendDst,false)},
    .alpha={.operation=wgpu::BlendOperation::Add,.srcFactor=wgpu::BlendFactor::One,
            .dstFactor=wgpu::BlendFactor::OneMinusSrcAlpha},
  };
  const wgpu::ColorTargetState color{.format=format,.blend=key.blend?&blend:nullptr};
  const wgpu::FragmentState fragment{.module=module,.entryPoint="fs",.targetCount=1,.targets=&color};
  const bool stencil=target.depthFormat==wgpu::TextureFormat::Depth24PlusStencil8;
  const wgpu::StencilFaceState mark{.compare=wgpu::CompareFunction::Always,
    .passOp=stencil?wgpu::StencilOperation::Replace:wgpu::StencilOperation::Keep};
  const wgpu::DepthStencilState depth{.format=target.depthFormat,.depthWriteEnabled=key.depthWrite,
    .depthCompare=reversed?wgpu::CompareFunction::GreaterEqual:wgpu::CompareFunction::LessEqual,
    .stencilFront=mark,.stencilBack=mark,.stencilReadMask=1,.stencilWriteMask=stencil?1u:0u};
  wgpu::RenderPipelineDescriptor desc{};desc.label=multiview?"Cockpit item (multiview)":"Cockpit item";
  desc.layout=multiview?pipelineLayoutMultiview:pipelineLayout;
  desc.vertex={.module=module,.entryPoint="vs",.bufferCount=1,.buffers=&vertices};
  desc.fragment=&fragment;desc.depthStencil=&depth;desc.multisample.count=target.msaaSamples;
  desc.primitive.topology=wgpu::PrimitiveTopology::TriangleList;
  // Same winding as Aurora's GX pipelines: GX front faces are clockwise.
  desc.primitive.frontFace=wgpu::FrontFace::CW;
  desc.primitive.cullMode=key.cull==1?wgpu::CullMode::Front:key.cull==2?wgpu::CullMode::Back:wgpu::CullMode::None;
#if defined(WGPU_DAWN_RENDER_PIPELINE_MULTIVIEW_INIT)
  wgpu::DawnRenderPipelineMultiview viewMask{};viewMask.viewMask=stereo_multiview::kViewMask;
  if(multiview) desc.nextInChain=&viewMask;
#endif
  cache.emplace_back(key,g_device.CreateRenderPipeline(&desc));
  return cache.back().second;
}

inline bool finite_matrix(const float* m) {
  for(int i=0;i<12;++i) {uint32_t bits;std::memcpy(&bits,m+i,4);if((bits&0x7f800000u)==0x7f800000u) return false;}
  return true;
}
inline void compose(const float* a,const float* b,float* out) {
  for(int r=0;r<3;++r) for(int c=0;c<4;++c) {
    out[r*4+c]=c==3?a[r*4+3]:0;
    for(int k=0;k<3;++k) out[r*4+c]+=a[r*4+k]*b[k*4+c];
  }
}

// The held item's frame in the seated frame (+X right, +Y up, -Z forward). It
// stays upright whatever the hand's roll and pitch, sits just above the palm and
// turns only with the hand's heading, facing back along it: the item's front
// (+Z) faces the player while the fingers point ahead. The fingers run along
// grip -Y and along the palm joint's -Z.
inline bool seat_from_item(const AuroraCockpitHand& hand,std::array<float,12>& out) {
  const float* pose=hand.jointsValid?hand.seatFromJoint[0]:hand.seatFromGrip;
  if(!finite_matrix(pose)) return false;
  const int fingers=hand.jointsValid?2:1;
  // The heading only. Within ~9 degrees of pointing straight up or down it
  // fades to straight ahead instead of spinning the item.
  const float x=-pose[fingers],z=-pose[8+fingers];
  const float length=std::sqrt(x*x+z*z),weight=std::clamp(length/0.15f,0.0f,1.0f);
  float headingX=weight*x/std::max(length,1e-6f),headingZ=weight*z/std::max(length,1e-6f)-(1-weight);
  float heading=std::sqrt(headingX*headingX+headingZ*headingZ);
  if(!(heading>1e-4f)) { headingX=0;headingZ=-1;heading=1; }
  const float frontX=-headingX/heading,frontZ=-headingZ/heading;
  constexpr float lift=0.05f;
  out={frontZ,0,frontX,pose[3], 0,1,0,pose[7]+lift, -frontX,0,frontZ,pose[11]};
  return true;
}
// The held item's largest side, in metres, when the cockpit gives no usable
// size ([vr] cockpit_item_size).
inline constexpr float kDefaultItemSize=0.28f;
inline float item_size(const AuroraCockpit& cockpit) {
  const float size=cockpit.itemSize;
  return size>=0.02f && size<=1.0f ? size : kDefaultItemSize;
}
// Scales a model to `size` metres across its largest side and stands it on the
// item frame's origin, centred.
inline std::array<float,12> item_from_model(const data::Model& model,float size) {
  const float span=std::max({model.maximum.x-model.minimum.x,model.maximum.y-model.minimum.y,
                             model.maximum.z-model.minimum.z});
  if(!(span>0.001f) || !std::isfinite(span)) return {};
  const float k=size/span;
  const float cx=(model.minimum.x+model.maximum.x)*0.5f,cz=(model.minimum.z+model.maximum.z)*0.5f;
  return {k,0,0,-k*cx, 0,k,0,-k*model.minimum.y, 0,0,k,-k*cz};
}

inline void render(const wgpu::RenderPassEncoder& pass,const StereoReplayFrame& frame,uint32_t eye,
                   float sceneZ,float sceneConstant) {
  if(!frame.cockpit.active || !(frame.cockpit.unitsPerMeter>0) || eye>1) return;
  refresh_archive();
  if(!gpuArchive) return;
  prepare_layout();
  // Upload on registration, before the first roulette settles, so receiving an
  // item does not pause the frame on texture uploads.
  for(size_t i=0;i<gpuArchive->models.size();++i)
    if(!gpuReady[i] && gpuArchive->models[i].valid()) prepare_model(i);
  const auto& item=frame.cockpitItem;
  if(!item.valid || item.hand>1 || item.count==0 || item.count>3) return;
  const int index=data::model_index(item.id);
  if(index<0 || !gpuArchive->models[index].valid()) return;
  const auto& hand=frame.cockpit.hands[item.hand];
  std::array<float,12> seatFromItem;
  if(!hand.tracked || !seat_from_item(hand,seatFromItem)) return;
  const auto& model=gpuArchive->models[index];
  const auto itemFromModel=item_from_model(model,item_size(frame.cockpit));
  if(!(itemFromModel[0]>0)) return;
  GpuFrame uniform{};
  compose(seatFromItem.data(),itemFromModel.data(),uniform.seatFromModel);
  compose(frame.cockpit.eyeFromSeat[eye],uniform.seatFromModel,uniform.eyeFromModel);
  const auto& projection=frame.eyes[eye].projection;
  const float projectionRow[4]{projection.m0[0],projection.m0[2],projection.m1[1],projection.m1[2]};
  const float depth[4]{sceneZ,sceneConstant/std::max(frame.cockpit.unitsPerMeter,0.001f),itemFromModel[0],0};
  std::memcpy(uniform.projection,projectionRow,sizeof(projectionRow));
  std::memcpy(uniform.depth,depth,sizeof(depth));
  webgpu::g_queue.WriteBuffer(frameBuffers[eye],0,&uniform,sizeof(uniform));
  const auto& gpu=gpuModels[index];
  pass.SetVertexBuffer(0,gpu.vertices);
  pass.SetBindGroup(1,frameGroups[eye],0,nullptr);
  const bool reversed=sceneConstant>0;
  uint32_t start=0;
  for(const auto& part:model.parts) {
    const auto count=static_cast<uint32_t>(part.vertices.size());
    const auto& material=model.materials[part.material];
    if(material.cull!=3) {
      const PipelineKey key{material.cull,material.blendSrc,material.blendDst,material.blend,material.subtract,
                            material.depthWrite};
      pass.SetPipeline(pipeline(key,frame,eye,reversed));
      pass.SetBindGroup(0,gpu.materials[part.material],0,nullptr);
      pass.Draw(count,1,start,0);
    }
    start+=count;
  }
}

// Both eyes at once inside a multiview pass (gfx/stereo_multiview.hpp): the two eyes' frames in one
// uniform, the shader taking the view's.
inline void render_multiview(const wgpu::RenderPassEncoder& pass,const StereoReplayFrame& frame,
                             float sceneZ,float sceneConstant) {
  if(!frame.cockpit.active || !(frame.cockpit.unitsPerMeter>0) || !frame.multiview) return;
  refresh_archive();
  if(!gpuArchive) return;
  prepare_layout();
  if(!frameGroupMultiview || !shaderMultiview) return;
  for(size_t i=0;i<gpuArchive->models.size();++i)
    if(!gpuReady[i] && gpuArchive->models[i].valid()) prepare_model(i);
  const auto& item=frame.cockpitItem;
  if(!item.valid || item.hand>1 || item.count==0 || item.count>3) return;
  const int index=data::model_index(item.id);
  if(index<0 || !gpuArchive->models[index].valid()) return;
  const auto& hand=frame.cockpit.hands[item.hand];
  std::array<float,12> seatFromItem;
  if(!hand.tracked || !seat_from_item(hand,seatFromItem)) return;
  const auto& model=gpuArchive->models[index];
  const auto itemFromModel=item_from_model(model,item_size(frame.cockpit));
  if(!(itemFromModel[0]>0)) return;
  std::array<GpuFrame,2> uniforms{};
  for(uint32_t eye=0;eye<2;++eye) {
    auto& uniform=uniforms[eye];
    compose(seatFromItem.data(),itemFromModel.data(),uniform.seatFromModel);
    compose(frame.cockpit.eyeFromSeat[eye],uniform.seatFromModel,uniform.eyeFromModel);
    const auto& projection=frame.eyes[eye].projection;
    const float projectionRow[4]{projection.m0[0],projection.m0[2],projection.m1[1],projection.m1[2]};
    const float depth[4]{sceneZ,sceneConstant/std::max(frame.cockpit.unitsPerMeter,0.001f),itemFromModel[0],0};
    std::memcpy(uniform.projection,projectionRow,sizeof(projectionRow));
    std::memcpy(uniform.depth,depth,sizeof(depth));
  }
  webgpu::g_queue.WriteBuffer(frameBufferMultiview,0,uniforms.data(),sizeof(uniforms));
  const auto& gpu=gpuModels[index];
  pass.SetVertexBuffer(0,gpu.vertices);
  pass.SetBindGroup(1,frameGroupMultiview,0,nullptr);
  const bool reversed=sceneConstant>0;
  uint32_t start=0;
  for(const auto& part:model.parts) {
    const auto count=static_cast<uint32_t>(part.vertices.size());
    const auto& material=model.materials[part.material];
    if(material.cull!=3) {
      const PipelineKey key{material.cull,material.blendSrc,material.blendDst,material.blend,material.subtract,
                            material.depthWrite};
      const auto& pipe=pipeline(key,frame,0,reversed,true);
      if(!pipe) { start+=count; continue; }
      pass.SetPipeline(pipe);
      pass.SetBindGroup(0,gpu.materials[part.material],0,nullptr);
      pass.Draw(count,1,start,0);
    }
    start+=count;
  }
}

} // namespace aurora::gfx::cockpit_item
