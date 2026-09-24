#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

// How an immersive eye replays the frame's main-EFB passes (gfx::render_stereo_eye).
//
// The mono render ends a render pass at every GX copy, because the copy reads what was drawn
// before it. An eye never performs those copies: it samples what the mono render baked. So it can
// keep drawing in the render pass it has open across them. On a tiled GPU every split stores the
// whole eye and loads it back, and a fragment density map makes that load even costlier (DolphinXR
// measured foveation as a net loss on Mario Kart Wii for exactly this reason).
namespace aurora::gfx::eye_pass_plan {

// What the plan needs from one recorded pass.
struct PassSummary {
  // The pass draws into the main EFB, which an eye replays into its own target.
  bool efbTarget = false;
  // Attachment clears of the whole target (all of RGBA for color).
  bool clearColor = false;
  bool clearDepth = false;
  // The continuation after a GXCopyDisp, whose clears are that copy's EFB reset.
  bool postCopyClear = false;
  bool hasCommands = false;
};

struct Step {
  // The recorded pass whose commands this step replays.
  uint32_t pass = 0;
  // Begin a render pass with these load ops; otherwise keep drawing in the open one.
  bool begin = false;
  bool clearColor = false;
  bool clearDepth = false;
  // Only an eye's first render pass clears its stencil, the VR cockpit's mask.
  bool clearStencil = false;
};

struct Plan {
  std::vector<Step> steps;
  // Recorded EFB passes up to the replay's last pass.
  uint32_t efbPasses = 0;
  // EFB passes whose pixels a later clear of the whole color and depth erases.
  uint32_t dead = 0;
  // Passes left with nothing to draw or clear.
  uint32_t empty = 0;
  // Render passes begun, and how many of them a clear of only color or only depth forced after
  // the first.
  uint32_t renderPasses = 0;
  uint32_t splits = 0;
};

// `lastPass` is the inclusive index of the last recorded pass to replay, or -1 for all of them.
// `skipCopyClears` drops a GXCopyDisp's EFB reset, as the eye replay does
// (set_stereo_skip_copy_clears).
inline void build(const PassSummary* passes, size_t count, int32_t lastPass, bool skipCopyClears, Plan& plan) {
  plan.steps.clear();
  plan.efbPasses = 0;
  plan.dead = 0;
  plan.empty = 0;
  plan.renderPasses = 0;
  plan.splits = 0;
  const size_t end = lastPass >= 0 ? std::min(count, static_cast<size_t>(lastPass) + 1) : count;
  const auto clears = [skipCopyClears](const PassSummary& pass, bool& color, bool& depth) {
    const bool dropped = skipCopyClears && pass.postCopyClear;
    color = pass.clearColor && !dropped;
    depth = pass.clearDepth && !dropped;
  };

  // The mono render still draws every pass for the copies the game samples; in the eye, anything
  // drawn before the last clear of both color and depth is erased by it.
  size_t first = 0;
  for (size_t i = 0; i < end; ++i) {
    if (!passes[i].efbTarget) {
      continue;
    }
    ++plan.efbPasses;
    bool color = false;
    bool depth = false;
    clears(passes[i], color, depth);
    if (color && depth) {
      first = i;
    }
  }

  for (size_t i = 0; i < end; ++i) {
    const PassSummary& pass = passes[i];
    if (!pass.efbTarget) {
      continue;
    }
    if (i < first) {
      ++plan.dead;
      continue;
    }
    bool color = false;
    bool depth = false;
    clears(pass, color, depth);
    if (!color && !depth && !pass.hasCommands) {
      ++plan.empty;
      continue;
    }
    // After the last full clear, only a clear of color alone or depth alone still needs a load op;
    // every other continuation keeps drawing where the previous pass left off.
    const bool begin = plan.renderPasses == 0 || color || depth;
    plan.steps.push_back(Step{
        .pass = static_cast<uint32_t>(i),
        .begin = begin,
        .clearColor = color,
        .clearDepth = depth,
        .clearStencil = begin && plan.renderPasses == 0,
    });
    if (begin) {
      ++plan.renderPasses;
    }
  }
  plan.splits = plan.renderPasses > 0 ? plan.renderPasses - 1 : 0;
}

} // namespace aurora::gfx::eye_pass_plan
