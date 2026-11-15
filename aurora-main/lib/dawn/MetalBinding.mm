#include "BackendBinding.hpp"

#import <Foundation/Foundation.h>
#include <SDL3/SDL_metal.h>
#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_video.h>
#include <TargetConditionals.h>

#if TARGET_OS_VISION
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#endif

namespace aurora::webgpu::utils {
#if !TARGET_OS_VISION
namespace {
constexpr const char* MetalViewProperty = "aurora.window.metal_view";

void SDLCALL DestroyMetalView(void*, void* value) {
  SDL_Metal_DestroyView(value);
}
} // namespace
#endif

std::shared_ptr<wgpu::ChainedStruct> SetupWindowAndGetSurfaceDescriptorCocoa(SDL_Window* window) {
#if TARGET_OS_VISION
  // Apple Vision Pro: SDL runs on its offscreen video driver (lib/window.cpp), so
  // there is no UIKit view to hang a Metal layer on, and nothing ever shows the
  // desktop image anyway: CompositorServices owns the display and the eyes reach
  // it through aurora/metal_interop.h. Dawn still wants a CAMetalLayer for its
  // surface, so it gets one that belongs to no view. Its drawables are acquired
  // and presented like any other layer's and simply never reach a screen.
  // One layer per process: a surface rebuild reconfigures it rather than
  // leaking a fresh one each time.
  static CAMetalLayer* layer = nil;
  if (layer == nil) {
    layer = [CAMetalLayer layer];
    layer.device = MTLCreateSystemDefaultDevice();
    layer.framebufferOnly = NO;
    layer.opaque = YES;
  }
  int width = 0;
  int height = 0;
  if (window != nullptr && SDL_GetWindowSizeInPixels(window, &width, &height) && width > 0 && height > 0) {
    layer.drawableSize = CGSizeMake(width, height);
  }
  auto desc = std::make_shared<wgpu::SurfaceSourceMetalLayer>();
  desc->layer = (__bridge void*)layer;
  return desc;
#else
  const auto properties = SDL_GetWindowProperties(window);
  if (!properties) {
    return nullptr;
  }
  auto view = SDL_GetPointerProperty(properties, MetalViewProperty, nullptr);
  if (!view) {
    view = SDL_Metal_CreateView(window);
    if (!view) {
      return nullptr;
    }
    // Own one view per window, not per WebGPU surface. Surface recovery must
    // preserve the UIKit root and its controls (and the Cocoa Metal subview).
    // SDL cleans window properties before destroying its native window.
    // The cleanup callback also runs if setting the property fails.
    if (!SDL_SetPointerPropertyWithCleanup(properties, MetalViewProperty, view, DestroyMetalView, nullptr)) {
      return nullptr;
    }
  }
  auto desc = std::make_shared<wgpu::SurfaceSourceMetalLayer>();
  desc->layer = SDL_Metal_GetLayer(view);
  if (!desc->layer) {
    SDL_ClearProperty(properties, MetalViewProperty);
    return nullptr;
  }
  return desc;
#endif
}
} // namespace aurora::webgpu::utils
