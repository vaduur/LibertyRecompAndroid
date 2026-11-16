#pragma once

// Frame budget arithmetic for the performance hint session (android_power.cpp),
// kept free of Android APIs so it can be tested on the host
// (android_power_budget_test.cpp).

#include <algorithm>
#include <cstdint>

namespace gta4::android_power {

// Period of an unlocked or unknown frame limit: the 30 FPS budget the session
// used before it followed the limit.
inline constexpr int64_t kDefaultFramePeriodNs = 33'333'333;

// One frame at gta4_frame_limit (0 = unlocked).
constexpr int64_t FramePeriodNs(uint32_t frame_limit) {
  if (frame_limit == 0 || frame_limit > 240) return kDefaultFramePeriodNs;
  return 1'000'000'000 / int64_t(frame_limit);
}

// The work target the session asks for: 90% of the frame period, and never
// more than android_perf_hint_target_ms. At 30 FPS with the default 30 ms cap
// both give 30 ms, the value the session always used.
constexpr int64_t TargetWorkNs(uint32_t frame_limit, double cap_ms) {
  const int64_t margin = FramePeriodNs(frame_limit) / 10 * 9;
  const int64_t cap = int64_t(cap_ms * 1e6);
  return std::max<int64_t>(1'000'000, std::min(cap, margin));
}

}  // namespace gta4::android_power
