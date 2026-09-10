#pragma once

#include <algorithm>
#include <cstdint>

namespace aurora::stereo {
// The current scene belongs to the VI presentation boundary. Delaying scene
// motion by one guest interval lets every display sample fall between known
// endpoints; head tracking is applied afterwards at its predicted display time.
inline float interpolation_weight(uint64_t displayTime, uint64_t boundary, uint64_t interval) noexcept {
  if (displayTime == 0 || boundary == 0 || interval == 0)
    return 1.0f;
  if (displayTime <= boundary)
    return 0.0f;
  return static_cast<float>(std::min(static_cast<double>(displayTime - boundary) / static_cast<double>(interval), 1.0));
}
} // namespace aurora::stereo
