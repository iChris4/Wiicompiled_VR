// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from heurazy's mario-kart-wii-VR-port (GPL-3.0-or-later).
//
// VR cockpit overlay: the synthetic steering wheel or handlebar (used when the
// vehicle's own wheel cannot be animated) and the tracked hands, drawn per eye
// in metres against the replayed scene's depth. See OPENXR.md, "Steering wheel
// and hand steering".
#pragma once
#include "common.hpp"
#include "cockpit_item.hpp"
#include "stereo_multiview.hpp"
#include "../webgpu/gpu.hpp"
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

namespace aurora::gfx::cockpit {
using V = std::array<float, 3>;
using M = std::array<float, 12>;
inline V add(V a, V b) { return {a[0]+b[0], a[1]+b[1], a[2]+b[2]}; }
inline V sub(V a, V b) { return {a[0]-b[0], a[1]-b[1], a[2]-b[2]}; }
inline V mul(V a, float b) { return {a[0]*b, a[1]*b, a[2]*b}; }
inline float dot(V a, V b) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
inline V cross(V a, V b) { return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]}; }
inline V norm(V a) { return mul(a, 1/std::sqrt(std::max(dot(a,a), 1e-10f))); }
inline V point(const float* m, V p) {
  return {m[0]*p[0]+m[1]*p[1]+m[2]*p[2]+m[3], m[4]*p[0]+m[5]*p[1]+m[6]*p[2]+m[7],
          m[8]*p[0]+m[9]*p[1]+m[10]*p[2]+m[11]};
}
inline M identity() { return {1,0,0,0,0,1,0,0,0,0,1,0}; }
inline M compose(const M& a, const M& b) {
  M result{};
  for(int r=0;r<3;++r) {
    for(int c=0;c<3;++c) for(int k=0;k<3;++k) result[r*4+c]+=a[r*4+k]*b[k*4+c];
    result[r*4+3]=a[r*4+3];
    for(int k=0;k<3;++k) result[r*4+3]+=a[r*4+k]*b[k*4+3];
  }
  return result;
}
inline M inverse(const M& m) {
  M out=identity();
  for(int r=0;r<3;++r) for(int c=0;c<3;++c) out[r*4+c]=m[c*4+r];
  const auto p=point(out.data(), {-m[3],-m[7],-m[11]});
  out[3]=p[0];out[7]=p[1];out[11]=p[2];return out;
}
inline M from_pose(const float* p) {
  const float x=p[0],y=p[1],z=p[2],w=p[3];
  return {1-2*(y*y+z*z),2*(x*y-z*w),2*(x*z+y*w),p[4],
          2*(x*y+z*w),1-2*(x*x+z*z),2*(y*z-x*w),p[5],
          2*(x*z-y*w),2*(y*z+x*w),1-2*(x*x+y*y),p[6]};
}
struct HandMesh {
  std::vector<AuroraVRHandVertex> vertices;
  std::vector<uint16_t> indices;
  std::array<M,26> bind{}, inverseBind{};
  std::array<int32_t,26> parents{};
};
inline std::mutex meshMutex;
inline std::array<std::shared_ptr<const HandMesh>,2> meshes;
struct Vertex { V position, color; };
inline void triangle(std::vector<Vertex>& vertices, V a, V b, V c, V color) {
  const V normal=norm(cross(sub(b,a),sub(c,a)));
  const float light=0.55f+0.45f*std::abs(dot(normal,norm({0.3f,0.8f,0.5f})));
  color=mul(color,light);
  vertices.insert(vertices.end(),{{a,color},{b,color},{c,color}});
}
inline void tube(std::vector<Vertex>& v, V a, V b, float radius, V color, int sides=8) {
  const auto direction=norm(sub(b,a));
  const auto u=norm(cross(direction,std::abs(direction[1])<0.9f?V{0,1,0}:V{1,0,0}));
  const auto w=cross(direction,u);
  for(int i=0;i<sides;++i) {
    const float t=float(i)*6.2831853f/sides, t1=float(i+1)*6.2831853f/sides;
    const V o=mul(add(mul(u,std::cos(t)),mul(w,std::sin(t))),radius);
    const V p=mul(add(mul(u,std::cos(t1)),mul(w,std::sin(t1))),radius);
    triangle(v,add(a,o),add(b,o),add(b,p),color);
    triangle(v,add(a,o),add(b,p),add(a,p),color);
    triangle(v,a,add(a,p),add(a,o),color);
    triangle(v,b,add(b,o),add(b,p),color);
  }
}
inline void ellipsoid(std::vector<Vertex>& vertices,V center,V radii,V color) {
  const auto surface=[&](int ring,int segment) {
    const float latitude=float(ring)*3.14159265f/6,longitude=float(segment)*6.2831853f/12;
    return add(center,{radii[0]*std::sin(latitude)*std::cos(longitude),radii[1]*std::cos(latitude),
        radii[2]*std::sin(latitude)*std::sin(longitude)});
  };
  for(int ring=0;ring<6;++ring) for(int segment=0;segment<12;++segment) {
    const auto a=surface(ring,segment),b=surface(ring+1,segment),c=surface(ring+1,segment+1),d=surface(ring,segment+1);
    if(ring>0) triangle(vertices,a,b,d,color);
    if(ring<5) triangle(vertices,b,c,d,color);
  }
}
// Rounded palm and individually articulated fingers, in the controller's grip
// space as OpenXR defines it: the origin is the palm centroid, -Z runs up the
// tube the curled fingers form (little finger towards thumb), and +X is normal
// to the palm - *away* from it on the left hand, *into* it on the right. That
// asymmetry is what makes both grips carry the same orientation when the hands
// hold a wheel symmetrically, so the fingers run along -Y on both, and it is
// the geometry across the palm that mirrors: fingers close towards +X on the
// left hand and -X on the right, with the thumb on the same side. Building the
// fingers on any other axis bends them out of the back of the hand (seen on a
// Quest 3 on 2026-09-22) or, for the right hand alone, points them at the
// player (seen on the PC on 2026-09-23).
inline void glove(std::vector<Vertex>& v, const AuroraCockpitHand& hand, int side) {
  const size_t start=v.size();
  const V white{0.91f,0.95f,1.0f};
  const float palm=side==0?1.0f:-1.0f; // hand 0 is the left one
  const float curl=std::clamp(hand.held?0.85f:hand.squeeze,0.0f,1.0f);
  // Thin through the palm's normal, a little wider across the knuckles than
  // the palm is long.
  ellipsoid(v,{0,0,0},{0.018f,0.043f,0.041f},white);
  for(int finger=0;finger<4;++finger) {
    // Index finger nearest the thumb (-Z), little finger last.
    V a{0.0f,-0.030f,-0.025f+finger*0.017f};
    const float length=finger==0||finger==3?0.021f:0.026f;
    for(int joint=0;joint<3;++joint) {
      const float angle=curl*(0.55f+joint*0.8f);
      V b=add(a,{palm*std::sin(angle)*length,-std::cos(angle)*length,0.0f});
      tube(v,a,b,0.008f,white);
      ellipsoid(v,b,{0.008f,0.008f,0.008f},white);a=b;
    }
  }
  // Thumb: out of the palm's thumb side, closing across the fingers.
  const V thumbKnuckle{palm*0.026f,-0.034f,-0.030f};
  tube(v,{palm*0.010f,-0.012f,-0.034f},thumbKnuckle,0.010f,white);
  tube(v,thumbKnuckle,{palm*(0.030f+0.014f*curl),-(0.052f-0.016f*curl),-0.020f},0.009f,white);
  for(size_t i=start;i<v.size();++i) v[i].position=point(hand.seatFromGrip,v[i].position);
}
// Blends the runtime mesh with one skinning matrix per joint (joints left out
// by `valid` do not pull), then places it: shifted by `offset` and carried by
// `seatFromMesh` when there is one.
inline void skin_mesh(std::vector<Vertex>& out, const HandMesh& mesh, const std::array<M,26>& skin,
                      const std::array<bool,26>& valid, V offset, const float* seatFromMesh) {
  std::vector<V> points(mesh.vertices.size());
  for(size_t i=0;i<points.size();++i) {
    const auto& v=mesh.vertices[i]; V p{}; float total=0;
    for(int w=0;w<4;++w) if(v.joints[w]>=0&&v.joints[w]<26&&valid[v.joints[w]]&&v.weights[w]>0) {
      p=add(p,mul(point(skin[v.joints[w]].data(),{v.position[0],v.position[1],v.position[2]}),v.weights[w]));
      total+=v.weights[w];
    }
    if(total>0) p=mul(p,1/total);
    p=add(p,offset);
    points[i]=seatFromMesh?point(seatFromMesh,p):p;
  }
  for(size_t i=0;i+2<mesh.indices.size();i+=3)
    triangle(out,points[mesh.indices[i]],points[mesh.indices[i+1]],points[mesh.indices[i+2]],{0.91f,0.95f,1.0f});
}
inline void runtime_hand(std::vector<Vertex>& out, const AuroraCockpitHand& hand, const HandMesh& mesh) {
  std::array<M,26> posed{}, skin{};
  std::array<bool,26> done{};
  const float curl=std::clamp(hand.held?0.85f:hand.squeeze,0.0f,1.0f);
  // Bind hierarchy is supplied by the runtime. Root and wrist stay rigid;
  // finger joints curl locally when controllers provide squeeze input.
  for(int pass=0;pass<26;++pass) for(int j=0;j<26;++j) {
    if(done[j]) continue;
    const int parent=mesh.parents[j];
    if(parent>=0&&parent<26&&!done[parent]) continue;
    M local=parent>=0&&parent<26?compose(mesh.inverseBind[parent],mesh.bind[j]):mesh.bind[j];
    const bool fingerJoint=j>=2 && j!=6 && j!=11 && j!=16 && j!=21;
    if(fingerJoint) {
      // OpenXR joints point -Z toward the fingertip and +Y out of the back
      // of the hand. Flexion is therefore negative about local X, for both
      // hands; positive angles bend the fingers backward on runtime meshes.
      const float a=-curl*(j<6?0.3f:0.75f),c=std::cos(a),s=std::sin(a);
      local=compose(local,M{1,0,0,0,0,c,-s,0,0,s,c,0});
    }
    posed[j]=parent>=0&&parent<26?compose(posed[parent],local):local;
    skin[j]=compose(mesh.inverseBind[1],compose(posed[j],mesh.inverseBind[j]));
    done[j]=true;
  }
  // The wrist sits behind the controller grip/palm origin.
  skin_mesh(out,mesh,skin,done,{0,0,0.04f},hand.seatFromGrip);
}
// The hand-tracking joints are all finite: the hand is drawn from them.
inline bool joints_finite(const AuroraCockpitHand& hand) {
  const auto finite=[](float value) {
    uint32_t bits; std::memcpy(&bits,&value,sizeof(bits)); return (bits&0x7f800000u)!=0x7f800000u;
  };
  for(int j=0;j<AURORA_VR_HAND_JOINT_COUNT;++j) {
    if(!finite(hand.jointRadii[j])) return false;
    for(float value : hand.seatFromJoint[j]) if(!finite(value)) return false;
  }
  return true;
}
inline M joint_matrix(const AuroraCockpitHand& hand, int joint) {
  M m; std::memcpy(m.data(),hand.seatFromJoint[joint],sizeof(m)); return m;
}
// The runtime mesh posed by the tracked joints themselves: the bind poses are
// in the mesh's own space, as xrLocateHandJointsEXT reports poses, so each
// joint's skinning matrix is its tracked pose times its inverse bind pose,
// already in the seated frame. Nothing curls and the grip plays no part.
inline void tracked_hand(std::vector<Vertex>& out, const AuroraCockpitHand& hand, const HandMesh& mesh) {
  std::array<M,26> skin{};
  std::array<bool,26> valid{};
  for(int j=0;j<26;++j) { skin[j]=compose(joint_matrix(hand,j),mesh.inverseBind[j]); valid[j]=true; }
  skin_mesh(out,mesh,skin,valid,{0,0,0},nullptr);
}
// Without a runtime mesh (PC runtimes report joints but no XR_FB mesh), the
// tracked joints as a skeleton: each finger a chain of tubes from the wrist to
// its tip, the joints' own radii, and a ball in the palm.
inline void joint_skeleton(std::vector<Vertex>& v, const AuroraCockpitHand& hand) {
  const V white{0.91f,0.95f,1.0f};
  const auto at=[&](int joint) -> V {
    return {hand.seatFromJoint[joint][3],hand.seatFromJoint[joint][7],hand.seatFromJoint[joint][11]};
  };
  const auto radius=[&](int joint) { return std::clamp(hand.jointRadii[joint],0.004f,0.02f); };
  constexpr int metacarpal[5]{2,6,11,16,21}, tip[5]{5,10,15,20,25};
  for(int finger=0;finger<5;++finger) {
    tube(v,at(1),at(metacarpal[finger]),radius(metacarpal[finger]),white);
    for(int joint=metacarpal[finger];joint<tip[finger];++joint) {
      tube(v,at(joint),at(joint+1),radius(joint+1),white);
      ellipsoid(v,at(joint+1),{radius(joint+1),radius(joint+1),radius(joint+1)},white);
    }
  }
  const float palm=std::clamp(hand.jointRadii[0],0.015f,0.03f);
  ellipsoid(v,at(0),{palm,palm,palm},white);
}
inline void build_geometry(const AuroraCockpit& cockpit, std::vector<Vertex>& vertices) {
  vertices.clear();vertices.reserve(12000);
  // The visible radius and position must match runtime/vr/steering_wheel.h.
  if (!cockpit.nativeWheel && cockpit.bike) {
    const float c=std::cos(cockpit.wheelAngle),s=std::sin(cockpit.wheelAngle);
    const auto barPoint=[&](float x,float y,float z) {
      return point(cockpit.seatFromHandlebar,{c*x+s*y,-s*x+c*y,z});
    };
    const float radius=cockpit.handlebarRadius;
    tube(vertices,barPoint(-radius,0,0),barPoint(radius,0,0),0.013f,{0.45f,0.48f,0.52f});
    for(float side:{-1.0f,1.0f})
      tube(vertices,barPoint(side*std::max(radius-0.10f,0.0f),0,0),barPoint(side*radius,0,0),0.024f,{0.12f,0.18f,0.19f});
    tube(vertices,barPoint(0,0,-0.13f),barPoint(0,0,0),0.023f,{0.12f,0.65f,0.61f});
  } else if (!cockpit.nativeWheel) {
  const auto rim=[&](float angle) -> V { return {0.18f*std::cos(angle),-0.30f+0.18f*std::sin(angle),-0.42f}; };
  for(int i=0;i<64;++i) {
    const float angle=float(i)*6.2831853f/64-cockpit.wheelAngle;
    const V color=i>=15&&i<=17?V{0.2f,0.9f,0.8f}:V{0.14f,0.17f,0.20f};
    tube(vertices,rim(angle),rim(angle+6.2831853f/64),0.016f,color,6);
  }
  for(float a : {0.0f,3.14159265f,4.71238898f})
    tube(vertices,{0,-0.30f,-0.42f},rim(a-cockpit.wheelAngle),0.011f,{0.45f,0.48f,0.52f});
  tube(vertices,{0,-0.30f,-0.445f},{0,-0.30f,-0.395f},0.035f,{0.12f,0.65f,0.61f},16);
  }
  std::array<std::shared_ptr<const HandMesh>,2> current;
  { std::lock_guard lock(meshMutex);current=meshes; }
  // A hand the game's character model draws (the character body) gets none
  // here; what it carries is drawn all the same (append_item_badge,
  // cockpit_item::render).
  for(int side=0;side<2;++side) if(cockpit.hands[side].tracked && !cockpit.hands[side].characterHand) {
    const auto& hand=cockpit.hands[side];
    const bool joints=hand.jointsValid && joints_finite(hand);
    if(current[side]) joints ? tracked_hand(vertices,hand,*current[side]) : runtime_hand(vertices,hand,*current[side]);
    else if(joints) joint_skeleton(vertices,hand);
    else glove(vertices,hand,side);
  }
}
inline void append_item_badge(const AuroraCockpit& cockpit,const AuroraCockpitItem& item,
                              std::vector<Vertex>& vertices) {
  if(!item.valid || item.hand>1 || item.count<1 || item.count>3 ||
     (item.id!=5 && item.id!=16 && item.id!=17 && item.id!=18) ||
     !cockpit.hands[item.hand].tracked || !cockpit_item::has_model(item.id)) return;
  const auto& hand=cockpit.hands[item.hand];
  if(hand.jointsValid && !joints_finite(hand)) return;
  M frame;
  if(!cockpit_item::seat_from_item(hand,frame)) return;
  // In the item's upright frame, beside the widest model on the hand's outer
  // side, facing the player like the item, a quarter of its size up.
  const float side=item.hand==0?-1.0f:1.0f;
  const size_t first=vertices.size();
  const float size=cockpit_item::item_size(cockpit);
  const V base{side*(0.5f*size+0.025f),0.25f*size,0.0f};
  ellipsoid(vertices,base,{0.016f,0.020f,0.004f},{0.05f,0.08f,0.13f});
  // A tiny raised seven-segment digit stays legible without creating another
  // textured game asset. One model is held for every triple inventory ID.
  const uint8_t digit=item.count==1?0x06:item.count==2?0x5b:0x4f;
  const V white{0.95f,0.98f,0.85f};
  const auto segment=[&](int bit,float x0,float y0,float x1,float y1) {
    if(digit&(1u<<bit)) tube(vertices,add(base,{x0,y0,0.005f}),add(base,{x1,y1,0.005f}),0.0016f,white,5);
  };
  segment(0,-0.007f, 0.010f, 0.007f, 0.010f);
  segment(1, 0.008f, 0.009f, 0.008f, 0.001f);
  segment(2, 0.008f,-0.001f, 0.008f,-0.009f);
  segment(3,-0.007f,-0.010f, 0.007f,-0.010f);
  segment(4,-0.008f,-0.009f,-0.008f,-0.001f);
  segment(5,-0.008f, 0.001f,-0.008f, 0.009f);
  segment(6,-0.007f, 0.0f, 0.007f, 0.0f);
  for(size_t i=first;i<vertices.size();++i) vertices[i].position=point(frame.data(),vertices[i].position);
}
inline std::vector<Vertex> geometry(const AuroraCockpit& cockpit) {
  std::vector<Vertex> result;build_geometry(cockpit,result);return result;
}
// A located hand that may carry the held item. With the character body drawing
// the hands, the item can be all the overlay draws, and cockpit_item::render
// keeps its models uploading ahead of the first roulette as it did with the
// hand drawn here.
inline bool carries_hand(const AuroraCockpit& cockpit) {
  return cockpit.hands[0].tracked || cockpit.hands[1].tracked;
}
inline std::atomic<uint64_t> meshRevision{1};
inline std::vector<Vertex> frameVertices;
inline AuroraCockpit cachedCockpit{};
inline AuroraCockpitItem cachedCockpitItem{};
inline uint64_t cachedMeshRevision=0;
inline wgpu::RenderPipeline pipeline;
// Both eyes at once in a multiview pass (gfx/stereo_multiview.hpp): the same with each eye's vertices
// in a vertex buffer slot of its own, rebuilt with `pipeline`.
inline wgpu::RenderPipeline multiviewPipeline;
struct SceneDepth {
  float z=0, constant=0;
  bool valid=false;
};
inline uint32_t pipelineSamples=0;
inline bool pipelineReversedDepth=false;
inline wgpu::TextureFormat pipelineFormat{}, pipelineDepthFormat{};
inline std::array<wgpu::Buffer,2> vertexBuffers;
inline std::array<uint64_t,2> vertexCapacity{};
inline void shutdown() { pipeline=nullptr;multiviewPipeline=nullptr;pipelineSamples=0;vertexBuffers={};vertexCapacity={};cachedMeshRevision=0;cachedCockpitItem={};frameVertices.clear();cockpit_item::shutdown(); }
// The pipeline for an eye target's attachments and depth direction; the multiview one takes both eyes'
// vertices as two vertex buffers and draws the view's.
inline wgpu::RenderPipeline make_pipeline(const ReplayTarget& target,bool reversedDepth,bool multiview) {
  using namespace webgpu;
  const auto format=g_graphicsConfig.surfaceConfiguration.format;
  wgpu::ShaderSourceWGSL source{};
  source.code=multiview?R"(
      enable chromium_experimental_multiview;
      struct Out { @builtin(position) position: vec4f, @location(0) color: vec3f };
      @vertex fn vs(@builtin(view_index) view: u32, @location(0) position0: vec4f, @location(1) color0: vec3f,
                    @location(2) position1: vec4f, @location(3) color1: vec3f) -> Out {
        var o: Out; let right = view == 1u;
        o.position=select(position0, position1, right); o.color=select(color0, color1, right); return o;
      }
      @fragment fn fs(i: Out) -> @location(0) vec4f { return vec4f(i.color,1); }
    )":R"(
      struct Out { @builtin(position) position: vec4f, @location(0) color: vec3f };
      @vertex fn vs(@location(0) position: vec4f, @location(1) color: vec3f) -> Out {
        var o: Out; o.position=position; o.color=color; return o;
      }
      @fragment fn fs(i: Out) -> @location(0) vec4f { return vec4f(i.color,1); }
    )";
  wgpu::ShaderModuleDescriptor md{};md.nextInChain=&source;md.label="VR cockpit hands and wheel";
  auto shader=g_device.CreateShaderModule(&md);
  const wgpu::VertexAttribute attrs[]={{.format=wgpu::VertexFormat::Float32x4,.offset=0,.shaderLocation=0},
    {.format=wgpu::VertexFormat::Float32x3,.offset=16,.shaderLocation=1}};
  const wgpu::VertexAttribute attrsRight[]={{.format=wgpu::VertexFormat::Float32x4,.offset=0,.shaderLocation=2},
    {.format=wgpu::VertexFormat::Float32x3,.offset=16,.shaderLocation=3}};
  const wgpu::VertexBufferLayout layouts[]={{.arrayStride=28,.attributeCount=2,.attributes=attrs},
    {.arrayStride=28,.attributeCount=2,.attributes=attrsRight}};
  const wgpu::ColorTargetState color{.format=format};
  const wgpu::FragmentState fragment{.module=shader,.entryPoint="fs",.targetCount=1,.targets=&color};
  const bool stencil=target.depthFormat==wgpu::TextureFormat::Depth24PlusStencil8;
  const wgpu::StencilFaceState mark{.compare=wgpu::CompareFunction::Always,
    .passOp=stencil?wgpu::StencilOperation::Replace:wgpu::StencilOperation::Keep};
  const wgpu::DepthStencilState depth{.format=target.depthFormat,.depthWriteEnabled=true,
    .depthCompare=reversedDepth?wgpu::CompareFunction::GreaterEqual:wgpu::CompareFunction::LessEqual,
    .stencilFront=mark,.stencilBack=mark,.stencilReadMask=1,.stencilWriteMask=stencil?1u:0u};
  wgpu::RenderPipelineDescriptor desc{};desc.label=multiview?"VR cockpit (multiview)":"VR cockpit";
  desc.vertex={.module=shader,.entryPoint="vs",.bufferCount=multiview?2u:1u,.buffers=layouts};
  desc.fragment=&fragment;desc.depthStencil=&depth;desc.multisample.count=target.msaaSamples;
  desc.primitive.topology=wgpu::PrimitiveTopology::TriangleList;
#if defined(WGPU_DAWN_RENDER_PIPELINE_MULTIVIEW_INIT)
  wgpu::DawnRenderPipelineMultiview viewMask{};viewMask.viewMask=stereo_multiview::kViewMask;
  if(multiview) desc.nextInChain=&viewMask;
#else
  if(multiview) return {};
#endif
  return g_device.CreateRenderPipeline(&desc);
}
inline void ensure_pipelines(const ReplayTarget& target,bool reversedDepth,bool multiview) {
  const auto format=webgpu::g_graphicsConfig.surfaceConfiguration.format;
  if(!pipeline||pipelineSamples!=target.msaaSamples||pipelineFormat!=format||pipelineReversedDepth!=reversedDepth||pipelineDepthFormat!=target.depthFormat) {
    pipeline=make_pipeline(target,reversedDepth,false);multiviewPipeline=nullptr;
    pipelineSamples=target.msaaSamples;pipelineFormat=format;pipelineReversedDepth=reversedDepth;pipelineDepthFormat=target.depthFormat;
  }
  if(multiview&&!multiviewPipeline) multiviewPipeline=make_pipeline(target,reversedDepth,true);
}
// Writes the eye's clip-space vertices into its buffer; null when there is nothing to draw.
inline const wgpu::Buffer* upload_eye_vertices(const StereoReplayFrame& frame,uint32_t eye,SceneDepth sceneDepth) {
  using namespace webgpu;
  const auto revision=meshRevision.load();
  if(cachedMeshRevision!=revision || std::memcmp(&cachedCockpit,&frame.cockpit,sizeof(AuroraCockpit))!=0 ||
     std::memcmp(&cachedCockpitItem,&frame.cockpitItem,sizeof(AuroraCockpitItem))!=0) {
    build_geometry(frame.cockpit,frameVertices);
    append_item_badge(frame.cockpit,frame.cockpitItem,frameVertices);
    cachedCockpit=frame.cockpit;cachedCockpitItem=frame.cockpitItem;cachedMeshRevision=revision;
  }
  const auto& vertices=frameVertices;
  if(vertices.empty()) return nullptr;
  struct ClipVertex { float p[4]; V color; };
  static std::vector<ClipVertex> clip;
  clip.resize(vertices.size());
  const auto& projection=frame.eyes[eye].projection;
  for(size_t i=0;i<clip.size();++i) {
    const auto p=point(frame.cockpit.eyeFromSeat[eye],vertices[i].position);
    // The original race near plane can sit beyond a close hand. Keep that
    // hand at the nearest representable depth instead of clipping it away.
    const float z=sceneDepth.z*p[2]+sceneDepth.constant/std::max(frame.cockpit.unitsPerMeter,0.001f);
    clip[i]={{projection.m0[0]*p[0]+projection.m0[2]*p[2],projection.m1[1]*p[1]+projection.m1[2]*p[2],
      std::clamp(z,0.0f,std::max(-p[2],0.0f)),-p[2]},vertices[i].color};
  }
  const uint64_t bytes=clip.size()*sizeof(ClipVertex);
  if (!vertexBuffers[eye] || vertexCapacity[eye]<bytes) {
    vertexCapacity[eye]=(bytes+65535)&~uint64_t(65535);
    const wgpu::BufferDescriptor bd{.label="VR cockpit vertices",.usage=wgpu::BufferUsage::Vertex|wgpu::BufferUsage::CopyDst,
      .size=vertexCapacity[eye]};
    vertexBuffers[eye]=g_device.CreateBuffer(&bd);
  }
  g_queue.WriteBuffer(vertexBuffers[eye],0,clip.data(),bytes);
  return &vertexBuffers[eye];
}
inline void render(wgpu::CommandEncoder& cmd,const StereoReplayFrame& frame,uint32_t eye,SceneDepth sceneDepth={},
                   const wgpu::RenderPassEncoder* existingPass=nullptr) {
  if(!frame.cockpit.active || !sceneDepth.valid) return;
  const auto& target=frame.eyes[eye].target;
  // The guest can reverse its viewport depth independently of Aurora's
  // global reversed-Z convention. The final 1/d coefficient is authoritative.
  const bool reversedDepth=sceneDepth.constant>0;
  ensure_pipelines(target,reversedDepth,false);
  const wgpu::Buffer* buffer=upload_eye_vertices(frame,eye,sceneDepth);
  if(!buffer && !carries_hand(frame.cockpit)) return;
  const wgpu::RenderPassColorAttachment attachment{.view=target.colorView,.resolveTarget=target.resolveView,
    .loadOp=wgpu::LoadOp::Load,.storeOp=wgpu::StoreOp::Store};
  const wgpu::RenderPassDepthStencilAttachment depth{.view=target.depthView,.depthLoadOp=wgpu::LoadOp::Load,
    .depthStoreOp=wgpu::StoreOp::Store,.depthClearValue=1.0f,
    .stencilLoadOp=target.depthFormat==wgpu::TextureFormat::Depth24PlusStencil8?wgpu::LoadOp::Load:wgpu::LoadOp::Undefined,
    .stencilStoreOp=target.depthFormat==wgpu::TextureFormat::Depth24PlusStencil8?wgpu::StoreOp::Store:wgpu::StoreOp::Undefined};
  const wgpu::RenderPassDescriptor pd{.label="VR cockpit overlay",.colorAttachmentCount=1,.colorAttachments=&attachment,.depthStencilAttachment=&depth};
  auto pass=existingPass?*existingPass:cmd.BeginRenderPass(&pd);
  pass.SetViewport(0,0,float(target.size.width),float(target.size.height),0,1);
  pass.SetScissorRect(0,0,target.size.width,target.size.height);
  // Mark only depth-visible samples; later virtual-screen draws test for zero.
  pass.SetStencilReference(1);
  if(buffer) {
    pass.SetPipeline(pipeline);pass.SetVertexBuffer(0,*buffer);pass.Draw(static_cast<uint32_t>(frameVertices.size()));
  }
  cockpit_item::render(pass,frame,eye,sceneDepth.z,sceneDepth.constant);
  pass.SetStencilReference(0);
  if(!existingPass) pass.End();
}
// Both eyes at once in a multiview pass over the two-layer targets (StereoReplayFrame::multiview): each
// eye's vertices in a vertex buffer slot of its own, the shader drawing the view's.
inline void render_multiview(wgpu::CommandEncoder& cmd,const StereoReplayFrame& frame,SceneDepth sceneDepth={},
                             const wgpu::RenderPassEncoder* existingPass=nullptr) {
  if(!frame.cockpit.active || !sceneDepth.valid || !frame.multiview) return;
  const auto& target=frame.eyes[0].target;
  const bool reversedDepth=sceneDepth.constant>0;
  ensure_pipelines(target,reversedDepth,true);
  if(!multiviewPipeline) return;
  const wgpu::Buffer* left=upload_eye_vertices(frame,0,sceneDepth);
  const wgpu::Buffer* right=upload_eye_vertices(frame,1,sceneDepth);
  const bool hasGeometry=left&&right;
  if(!hasGeometry && !carries_hand(frame.cockpit)) return;
#if defined(WGPU_DAWN_RENDER_PASS_MULTIVIEW_INIT)
  const bool stencil=target.depthFormat==wgpu::TextureFormat::Depth24PlusStencil8;
  const wgpu::RenderPassColorAttachment attachment{.view=frame.multiviewColorView,
    .loadOp=wgpu::LoadOp::Load,.storeOp=wgpu::StoreOp::Store};
  const wgpu::RenderPassDepthStencilAttachment depth{.view=frame.multiviewDepthView,.depthLoadOp=wgpu::LoadOp::Load,
    .depthStoreOp=wgpu::StoreOp::Store,.depthClearValue=1.0f,
    .stencilLoadOp=stencil?wgpu::LoadOp::Load:wgpu::LoadOp::Undefined,
    .stencilStoreOp=stencil?wgpu::StoreOp::Store:wgpu::StoreOp::Undefined};
  wgpu::DawnRenderPassMultiview viewMask{};viewMask.viewMask=stereo_multiview::kViewMask;
  const wgpu::RenderPassDescriptor pd{.nextInChain=&viewMask,.label="VR cockpit overlay (multiview)",
    .colorAttachmentCount=1,.colorAttachments=&attachment,.depthStencilAttachment=&depth};
  auto pass=existingPass?*existingPass:cmd.BeginRenderPass(&pd);
  pass.SetViewport(0,0,float(target.size.width),float(target.size.height),0,1);
  pass.SetScissorRect(0,0,target.size.width,target.size.height);
  pass.SetStencilReference(1);
  if(hasGeometry) {
    pass.SetPipeline(multiviewPipeline);pass.SetVertexBuffer(0,*left);pass.SetVertexBuffer(1,*right);
    pass.Draw(static_cast<uint32_t>(frameVertices.size()));
  }
  cockpit_item::render_multiview(pass,frame,sceneDepth.z,sceneDepth.constant);
  pass.SetStencilReference(0);
  if(!existingPass) pass.End();
#else
  (void)cmd;(void)existingPass;(void)hasGeometry;
#endif
}
} // namespace aurora::gfx::cockpit
