#include "stereo_mirror.hpp"

#include <gtest/gtest.h>

namespace aurora::stereo {

TEST(StereoMirror, RetainsEyesAcrossMissingPackets) {
  MirrorState state;
  EXPECT_EQ(state.Resolve(AURORA_STEREO_MIRROR_LEFT_EYE, true, false, false), MirrorPlan::Mono);
  EXPECT_EQ(state.Resolve(AURORA_STEREO_MIRROR_LEFT_EYE, true, true, true), MirrorPlan::LeftEye);
  for (int frame = 0; frame < 120; ++frame) {
    EXPECT_EQ(state.Resolve(AURORA_STEREO_MIRROR_LEFT_EYE, true, false, false), MirrorPlan::LeftEye);
  }
  // Live selection remains usable even without a new headset frame.
  EXPECT_EQ(state.Resolve(AURORA_STEREO_MIRROR_RIGHT_EYE, true, false, false), MirrorPlan::RightEye);
  EXPECT_EQ(state.Resolve(AURORA_STEREO_MIRROR_BOTH_EYES, true, false, false), MirrorPlan::BothEyes);
  EXPECT_EQ(state.Resolve(AURORA_STEREO_MIRROR_NORMAL, true, false, false), MirrorPlan::Mono);
  EXPECT_EQ(state.Resolve(AURORA_STEREO_MIRROR_LEFT_EYE, true, false, false), MirrorPlan::LeftEye);
}

TEST(StereoMirror, MenusAndSessionEndReplaceRetainedRaceView) {
  MirrorState state;
  EXPECT_EQ(state.Resolve(AURORA_STEREO_MIRROR_BOTH_EYES, true, true, true), MirrorPlan::BothEyes);
  EXPECT_EQ(state.Resolve(AURORA_STEREO_MIRROR_BOTH_EYES, true, true, false), MirrorPlan::Mono);
  EXPECT_EQ(state.Resolve(AURORA_STEREO_MIRROR_BOTH_EYES, true, false, false), MirrorPlan::Mono);
  EXPECT_EQ(state.Resolve(AURORA_STEREO_MIRROR_BOTH_EYES, true, true, true), MirrorPlan::BothEyes);
  EXPECT_EQ(state.Resolve(AURORA_STEREO_MIRROR_BOTH_EYES, false, false, false), MirrorPlan::Mono);
  EXPECT_EQ(state.Resolve(AURORA_STEREO_MIRROR_BOTH_EYES, true, false, false), MirrorPlan::Mono);
  EXPECT_EQ(state.Resolve(AURORA_STEREO_MIRROR_LEFT_EYE, true, true, true), MirrorPlan::LeftEye);
  state.Reset(); // Provider replacement must not reuse the previous session.
  EXPECT_EQ(state.Resolve(AURORA_STEREO_MIRROR_LEFT_EYE, true, false, false), MirrorPlan::Mono);
}

TEST(StereoMirror, NoneStaysBlackWithoutFreshFrames) {
  MirrorState state;
  EXPECT_EQ(state.Resolve(AURORA_STEREO_MIRROR_NONE, true, true, true), MirrorPlan::Black);
  EXPECT_EQ(state.Resolve(AURORA_STEREO_MIRROR_NONE, true, false, false), MirrorPlan::Black);
  EXPECT_EQ(state.Resolve(AURORA_STEREO_MIRROR_NONE, true, true, false), MirrorPlan::Black);
  EXPECT_EQ(state.Resolve(AURORA_STEREO_MIRROR_NONE, true, false, false), MirrorPlan::Black);
  EXPECT_EQ(state.Resolve(AURORA_STEREO_MIRROR_NONE, false, false, false), MirrorPlan::Mono);
}

} // namespace aurora::stereo
