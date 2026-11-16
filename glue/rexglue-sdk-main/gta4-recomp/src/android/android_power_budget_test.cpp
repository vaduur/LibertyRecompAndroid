// Host test of android_power_budget.h. Not part of the Android build:
//   c++ -std=c++20 android_power_budget_test.cpp -o /tmp/budget_test && /tmp/budget_test
#include "android_power_budget.h"

#include <cstdio>
#include <cstdlib>

using namespace gta4::android_power;

namespace {
int failures = 0;

void Expect(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}
}  // namespace

// Compile-time checks of the plain cases.
static_assert(FramePeriodNs(30) == 33'333'333);
static_assert(FramePeriodNs(60) == 16'666'666);
static_assert(FramePeriodNs(0) == kDefaultFramePeriodNs);
static_assert(FramePeriodNs(1000) == kDefaultFramePeriodNs);

int main() {
  // 30 FPS keeps the session's historical 30 ms target (within rounding).
  const int64_t at30 = TargetWorkNs(30, 30.0);
  Expect(at30 > 29'900'000 && at30 <= 30'000'000, "30 FPS target is ~30 ms");
  // Higher caps get 90% of their own period, not the 30 FPS budget.
  Expect(TargetWorkNs(40, 30.0) == 22'500'000, "40 FPS target is 22.5 ms");
  Expect(TargetWorkNs(60, 30.0) == 14'999'994, "60 FPS target is ~15 ms");
  Expect(TargetWorkNs(120, 30.0) == 7'499'997, "120 FPS target is ~7.5 ms");
  // The cvar still caps the target from above.
  Expect(TargetWorkNs(30, 20.0) == 20'000'000, "cvar caps the 30 FPS target");
  // Unlocked behaves like 30 FPS.
  Expect(TargetWorkNs(0, 30.0) == at30, "unlocked uses the 30 FPS budget");
  // Never zero or negative (the API rejects that).
  Expect(TargetWorkNs(30, 0.0) == 1'000'000, "target has a 1 ms floor");

  if (failures) return EXIT_FAILURE;
  std::puts("android_power_budget: all checks passed");
  return EXIT_SUCCESS;
}
