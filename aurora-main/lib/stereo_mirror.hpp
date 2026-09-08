#pragma once

#include <aurora/gfx.h>

namespace aurora::stereo {

enum class MirrorPlan { Mono, LeftEye, RightEye, BothEyes, Black };

// Owned by the frame worker, alongside the persistent eye textures. An absent
// XR packet means the headset is retaining its image, not switching to mono.
class MirrorState {
public:
  MirrorPlan Resolve(AuroraStereoMirrorView view, bool providerActive,
                     bool freshOutput, bool immersiveReplay) noexcept {
    if (!providerActive) {
      Reset();
      return MirrorPlan::Mono;
    }
    if (freshOutput) {
      m_immersive = immersiveReplay;
    }
    switch (view) {
    case AURORA_STEREO_MIRROR_NONE:
      return MirrorPlan::Black;
    case AURORA_STEREO_MIRROR_BOTH_EYES:
      return m_immersive ? MirrorPlan::BothEyes : MirrorPlan::Mono;
    case AURORA_STEREO_MIRROR_LEFT_EYE:
      return m_immersive ? MirrorPlan::LeftEye : MirrorPlan::Mono;
    case AURORA_STEREO_MIRROR_RIGHT_EYE:
      return m_immersive ? MirrorPlan::RightEye : MirrorPlan::Mono;
    default:
      return MirrorPlan::Mono;
    }
  }

  void Reset() noexcept { m_immersive = false; }

private:
  bool m_immersive = false;
};

} // namespace aurora::stereo
