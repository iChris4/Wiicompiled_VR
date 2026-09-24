#include "gfx/eye_pass_plan.hpp"

#include <gtest/gtest.h>

#include <vector>

namespace aurora::gfx::eye_pass_plan {
namespace {

PassSummary efb(bool clearColor, bool clearDepth, bool hasCommands = true, bool postCopyClear = false) {
  return PassSummary{
      .efbTarget = true,
      .clearColor = clearColor,
      .clearDepth = clearDepth,
      .postCopyClear = postCopyClear,
      .hasCommands = hasCommands,
  };
}

PassSummary offscreen() {
  return PassSummary{.efbTarget = false, .clearColor = true, .clearDepth = true, .hasCommands = true};
}

Plan build(const std::vector<PassSummary>& passes, int32_t lastPass = -1, bool skipCopyClears = true) {
  Plan plan;
  eye_pass_plan::build(passes.data(), passes.size(), lastPass, skipCopyClears, plan);
  return plan;
}

TEST(EyePassPlan, ContinuationsAfterCopiesShareOneRenderPass) {
  const Plan plan = build({efb(true, true), efb(false, false), efb(false, false), efb(false, false)});
  ASSERT_EQ(plan.steps.size(), 4u);
  EXPECT_EQ(plan.efbPasses, 4u);
  EXPECT_EQ(plan.renderPasses, 1u);
  EXPECT_EQ(plan.splits, 0u);
  EXPECT_TRUE(plan.steps[0].begin);
  EXPECT_TRUE(plan.steps[0].clearColor);
  EXPECT_TRUE(plan.steps[0].clearDepth);
  EXPECT_TRUE(plan.steps[0].clearStencil);
  for (size_t i = 1; i < plan.steps.size(); ++i) {
    EXPECT_EQ(plan.steps[i].pass, i);
    EXPECT_FALSE(plan.steps[i].begin);
  }
}

TEST(EyePassPlan, FullClearErasesEverythingDrawnBeforeIt) {
  // A copy that clears the whole EFB: the eye never shows what came before it.
  const Plan plan = build({efb(true, true), efb(false, false), efb(true, true), efb(false, false)});
  EXPECT_EQ(plan.dead, 2u);
  EXPECT_EQ(plan.renderPasses, 1u);
  ASSERT_EQ(plan.steps.size(), 2u);
  EXPECT_EQ(plan.steps[0].pass, 2u);
  EXPECT_TRUE(plan.steps[0].begin);
  EXPECT_TRUE(plan.steps[0].clearColor);
  EXPECT_TRUE(plan.steps[0].clearDepth);
  // The stencil is cleared where the eye actually starts, so a cockpit mask drawn in a pass that
  // gets erased can no longer punch holes in the HUD.
  EXPECT_TRUE(plan.steps[0].clearStencil);
  EXPECT_EQ(plan.steps[1].pass, 3u);
  EXPECT_FALSE(plan.steps[1].begin);
}

TEST(EyePassPlan, ClearOfOnlyColorOrOnlyDepthSplits) {
  const Plan plan = build({efb(true, true), efb(false, true), efb(false, false), efb(true, false)});
  EXPECT_EQ(plan.dead, 0u);
  EXPECT_EQ(plan.renderPasses, 3u);
  EXPECT_EQ(plan.splits, 2u);
  ASSERT_EQ(plan.steps.size(), 4u);
  EXPECT_TRUE(plan.steps[1].begin);
  EXPECT_FALSE(plan.steps[1].clearColor);
  EXPECT_TRUE(plan.steps[1].clearDepth);
  EXPECT_FALSE(plan.steps[1].clearStencil);
  EXPECT_FALSE(plan.steps[2].begin);
  EXPECT_TRUE(plan.steps[3].begin);
  EXPECT_TRUE(plan.steps[3].clearColor);
  EXPECT_FALSE(plan.steps[3].clearDepth);
  EXPECT_FALSE(plan.steps[3].clearStencil);
}

TEST(EyePassPlan, DisplayCopyResetFollowsTheCopyClearSwitch) {
  // The continuation after GXCopyDisp holds only that copy's EFB reset.
  const std::vector passes{efb(true, true), efb(true, true, false, true)};
  const Plan skipped = build(passes, -1, true);
  EXPECT_EQ(skipped.dead, 0u);
  EXPECT_EQ(skipped.empty, 1u);
  ASSERT_EQ(skipped.steps.size(), 1u);
  EXPECT_EQ(skipped.steps[0].pass, 0u);

  // Replayed raw, the reset erases the frame, as the legacy replay's black eye shows.
  const Plan raw = build(passes, -1, false);
  EXPECT_EQ(raw.dead, 1u);
  ASSERT_EQ(raw.steps.size(), 1u);
  EXPECT_EQ(raw.steps[0].pass, 1u);
  EXPECT_TRUE(raw.steps[0].clearColor);
  EXPECT_TRUE(raw.steps[0].clearDepth);
}

TEST(EyePassPlan, OffscreenPassesNeitherSplitNorEraseTheEye) {
  const Plan plan = build({efb(true, true), offscreen(), efb(false, false)});
  EXPECT_EQ(plan.efbPasses, 2u);
  EXPECT_EQ(plan.dead, 0u);
  EXPECT_EQ(plan.renderPasses, 1u);
  ASSERT_EQ(plan.steps.size(), 2u);
  EXPECT_EQ(plan.steps[0].pass, 0u);
  EXPECT_EQ(plan.steps[1].pass, 2u);
  EXPECT_FALSE(plan.steps[1].begin);
}

TEST(EyePassPlan, StopsAtTheLastReplayedPass) {
  const std::vector passes{efb(true, true), efb(false, false), efb(true, true)};
  const Plan cut = build(passes, 1);
  EXPECT_EQ(cut.efbPasses, 2u);
  EXPECT_EQ(cut.dead, 0u);
  ASSERT_EQ(cut.steps.size(), 2u);
  EXPECT_EQ(cut.steps[1].pass, 1u);

  const Plan whole = build(passes, -1);
  EXPECT_EQ(whole.efbPasses, 3u);
  EXPECT_EQ(whole.dead, 2u);
  ASSERT_EQ(whole.steps.size(), 1u);
  EXPECT_EQ(whole.steps[0].pass, 2u);

  const Plan beyond = build(passes, 10);
  EXPECT_EQ(beyond.efbPasses, 3u);
}

TEST(EyePassPlan, SkipsEmptyPassesButKeepsClears) {
  const Plan plan = build({efb(true, true, false), efb(false, false, false), efb(false, false)});
  EXPECT_EQ(plan.empty, 1u);
  EXPECT_EQ(plan.renderPasses, 1u);
  ASSERT_EQ(plan.steps.size(), 2u);
  EXPECT_EQ(plan.steps[0].pass, 0u);
  EXPECT_TRUE(plan.steps[0].clearColor);
  EXPECT_EQ(plan.steps[1].pass, 2u);
  EXPECT_FALSE(plan.steps[1].begin);
}

TEST(EyePassPlan, AResumedFrameStartsByLoadingTheEye) {
  const Plan plan = build({efb(false, false), efb(false, false)});
  EXPECT_EQ(plan.renderPasses, 1u);
  ASSERT_EQ(plan.steps.size(), 2u);
  EXPECT_TRUE(plan.steps[0].begin);
  EXPECT_FALSE(plan.steps[0].clearColor);
  EXPECT_FALSE(plan.steps[0].clearDepth);
  EXPECT_TRUE(plan.steps[0].clearStencil);
  EXPECT_FALSE(plan.steps[1].begin);
}

TEST(EyePassPlan, NothingToReplay) {
  const Plan none = build({offscreen()});
  EXPECT_TRUE(none.steps.empty());
  EXPECT_EQ(none.efbPasses, 0u);
  EXPECT_EQ(none.renderPasses, 0u);
  EXPECT_EQ(none.splits, 0u);

  const Plan empty = build({});
  EXPECT_TRUE(empty.steps.empty());
}

TEST(EyePassPlan, RebuildingReusesThePlan) {
  Plan plan;
  const std::vector first{efb(true, true), efb(false, true), efb(true, true), efb(false, false)};
  eye_pass_plan::build(first.data(), first.size(), -1, true, plan);
  const std::vector second{efb(true, true), efb(false, false)};
  eye_pass_plan::build(second.data(), second.size(), -1, true, plan);
  EXPECT_EQ(plan.efbPasses, 2u);
  EXPECT_EQ(plan.dead, 0u);
  EXPECT_EQ(plan.empty, 0u);
  EXPECT_EQ(plan.renderPasses, 1u);
  EXPECT_EQ(plan.splits, 0u);
  EXPECT_EQ(plan.steps.size(), 2u);
}

} // namespace
} // namespace aurora::gfx::eye_pass_plan
