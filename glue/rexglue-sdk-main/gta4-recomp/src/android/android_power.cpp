// Power and heat: Android's performance hint (ADPF) and thermal headroom.
//
// Left alone, the CPU governor sees busy threads and raises the clocks of
// their cores toward the maximum, although a frame only has to be ready every
// 33 ms: the extra speed turns into idle time and heat. A performance hint
// session tells the system how long each frame's work took against its
// target, so it can pick the lowest clocks that still meet the target (and
// boost early when a frame runs long). The session holds the busiest threads
// of the process, re-ranked every two seconds like the thread pinning, and
// once per frame period it reports the largest CPU time any of them used.
// The frame period and the target follow gta4_frame_limit, so a 40 or 60 FPS
// cap gets a 25 or 16.7 ms budget instead of the 30 FPS one. When the busiest
// threads change, Android 14+ moves the session to the new threads
// (APerformanceHint_setThreads); older versions get a new session.
//
// Thermal headroom (0 = cool, 1 = the device starts throttling) is read every
// two seconds and logged. With android_thermal_guard the draw distance is
// lowered before the device throttles and restored once it has cooled down,
// trading distant detail for steady clocks instead of a throttled frame rate.
// Devices without a thermal HAL report no headroom (the Retroid Pocket 5, for
// one); the guard then follows the GPU temperature instead.
//
// Both APIs are looked up at run time (performance hints need Android 13,
// thermal headroom Android 11) and need the device's power and thermal HALs;
// without them the hints are skipped.
#include <dirent.h>
#include <dlfcn.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include <android/log.h>

#include <rex/cvar.h>

#include "android_power_budget.h"

REXCVAR_DEFINE_BOOL(android_perf_hint, true, "GTA IV/Performance",
                    "Report each frame's work to Android's performance hint API so the CPU "
                    "clocks follow the frame budget instead of running at the maximum");
REXCVAR_DEFINE_DOUBLE(android_perf_hint_target_ms, 30.0, "GTA IV/Performance",
                      "Longest work time per frame the performance hint asks for; the target is "
                      "the smaller of this and 90% of the frame limit's period (30 ms at 30 FPS)")
    .range(8.0, 33.0);
REXCVAR_DEFINE_BOOL(android_thermal_guard, false, "GTA IV/Performance",
                    "Lower the draw distance when Android forecasts thermal throttling and "
                    "restore it after the device has cooled down");
REXCVAR_DEFINE_DOUBLE(android_thermal_guard_hot_c, 78.0, "GTA IV/Performance",
                      "Thermal guard without thermal headroom: GPU temperature (C) that lowers "
                      "the draw distance")
    .range(50.0, 110.0);
REXCVAR_DEFINE_DOUBLE(android_thermal_guard_cool_c, 68.0, "GTA IV/Performance",
                      "Thermal guard without thermal headroom: GPU temperature (C) that restores "
                      "the draw distance")
    .range(40.0, 100.0);

REXCVAR_DECLARE(double, gta4_draw_distance_scale);
REXCVAR_DECLARE(uint32_t, gta4_frame_limit);

namespace {

using APerformanceHintManager = struct APerformanceHintManager;
using APerformanceHintSession = struct APerformanceHintSession;
using AThermalManager = struct AThermalManager;

struct Api {
  APerformanceHintManager* (*get_manager)() = nullptr;
  APerformanceHintSession* (*create_session)(APerformanceHintManager*, const int32_t*, size_t,
                                             int64_t) = nullptr;
  int (*update_target)(APerformanceHintSession*, int64_t) = nullptr;
  int (*report_actual)(APerformanceHintSession*, int64_t) = nullptr;
  void (*close_session)(APerformanceHintSession*) = nullptr;
  int (*set_threads)(APerformanceHintSession*, const pid_t*, size_t) = nullptr;  // API 34
  AThermalManager* (*thermal_acquire)() = nullptr;
  float (*thermal_headroom)(AThermalManager*, int) = nullptr;
};

Api LoadApi() {
  Api api;
  void* library = dlopen("libandroid.so", RTLD_NOW);
  if (!library) return api;
  const auto find = [library](auto& function, const char* name) {
    function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(dlsym(library, name));
  };
  find(api.get_manager, "APerformanceHint_getManager");
  find(api.create_session, "APerformanceHint_createSession");
  find(api.update_target, "APerformanceHint_updateTargetWorkDuration");
  find(api.report_actual, "APerformanceHint_reportActualWorkDuration");
  find(api.close_session, "APerformanceHint_closeSession");
  find(api.set_threads, "APerformanceHint_setThreads");
  find(api.thermal_acquire, "AThermal_acquireManager");
  find(api.thermal_headroom, "AThermal_getThermalHeadroom");
  return api;
}

// CPU time a thread of this process has used (CPUCLOCK_SCHED, per thread).
int64_t ThreadCpuNanoseconds(pid_t tid) {
  const clockid_t clock = clockid_t((~unsigned(tid) << 3) | 6u);
  timespec time{};
  if (clock_gettime(clock, &time) != 0) return -1;
  return int64_t(time.tv_sec) * 1000000000 + time.tv_nsec;
}

// Adreno GPU temperature in degrees C (kgsl reports millidegrees); NaN if absent.
float GpuTemperature() {
  float value = __builtin_nanf("");
  if (FILE* file = std::fopen("/sys/class/kgsl/kgsl-3d0/temp", "r")) {
    long raw = 0;
    if (std::fscanf(file, "%ld", &raw) == 1) value = raw > 1000 ? float(raw) / 1000.0f : float(raw);
    std::fclose(file);
  }
  return value;
}

// The busiest threads over the last interval, by CPU time.
std::vector<pid_t> BusiestThreads(std::unordered_map<pid_t, int64_t>& previous, size_t count) {
  std::vector<std::pair<int64_t, pid_t>> usage;
  std::unordered_map<pid_t, int64_t> current;
  if (DIR* directory = opendir("/proc/self/task")) {
    while (dirent* entry = readdir(directory)) {
      const pid_t tid = pid_t(std::atoi(entry->d_name));
      if (tid <= 0) continue;
      const int64_t time = ThreadCpuNanoseconds(tid);
      if (time < 0) continue;
      current[tid] = time;
      const auto old = previous.find(tid);
      if (old != previous.end() && time > old->second) usage.emplace_back(time - old->second, tid);
    }
    closedir(directory);
  }
  previous.swap(current);
  std::sort(usage.rbegin(), usage.rend());
  std::vector<pid_t> result;
  for (size_t i = 0; i < usage.size() && i < count; ++i) {
    // Threads that used under 5% of the interval carry no frame work.
    if (usage[i].first < 100'000'000) break;
    result.push_back(usage[i].second);
  }
  std::sort(result.begin(), result.end());
  return result;
}

void PowerLoop() {
  const Api api = LoadApi();
  const bool hints = api.get_manager && api.create_session && api.update_target &&
                     api.report_actual && api.close_session;
  // AThermal_acquireManager is API 30, AThermal_getThermalHeadroom API 31:
  // Android 11 has the manager without the headroom (issue #58).
  AThermalManager* thermal =
      api.thermal_acquire && api.thermal_headroom ? api.thermal_acquire() : nullptr;
  APerformanceHintManager* manager = hints ? api.get_manager() : nullptr;
  __android_log_print(ANDROID_LOG_WARN, "LibertyRecomp", "power: performance hints %s, thermal headroom %s",
                      manager ? "available" : "unavailable", thermal ? "available" : "unavailable");

  using Clock = std::chrono::steady_clock;
  // Re-read every tick: the in-game Display menu changes the frame limit live.
  const auto frame_period = [] {
    return std::chrono::nanoseconds(
        gta4::android_power::FramePeriodNs(REXCVAR_GET(gta4_frame_limit)));
  };
  const auto target_work = [] {
    return gta4::android_power::TargetWorkNs(REXCVAR_GET(gta4_frame_limit),
                                             REXCVAR_GET(android_perf_hint_target_ms));
  };
  constexpr auto kRankInterval = std::chrono::seconds(2);
  APerformanceHintSession* session = nullptr;
  std::vector<pid_t> threads;
  std::vector<int64_t> thread_times;
  std::unordered_map<pid_t, int64_t> rank_times;
  int64_t target_ns = 0;
  auto next_rank = Clock::now();
  auto next_tick = Clock::now() + frame_period();
  auto next_log = Clock::now() + std::chrono::seconds(10);
  float headroom = __builtin_nanf("");
  float gpu_temperature = __builtin_nanf("");
  double guard_saved_scale = 0.0;  // the draw distance the guard lowered; 0 = not lowered
  double reported_sum_ms = 0.0;
  uint32_t reported = 0;

  for (;;) {
    std::this_thread::sleep_until(next_tick);
    const auto now = Clock::now();
    const auto period = frame_period();
    next_tick += period;
    if (next_tick < now) next_tick = now + period;  // fell behind (suspended): resynchronize

    const bool enabled = manager && REXCVAR_GET(android_perf_hint);
    if (!enabled && session) {
      api.close_session(session);
      session = nullptr;
    }

    if (now >= next_rank) {
      next_rank = now + kRankInterval;
      const std::vector<pid_t> busiest = BusiestThreads(rank_times, 4);
      // "|| !session": the hint switched back on (live cvar) over the same threads.
      if (enabled && (busiest != threads || !session)) {
        threads = busiest;
        thread_times.assign(threads.size(), -1);
        // Moving a live session keeps what the system has learned about this
        // workload; a new session starts from scratch every two seconds.
        const bool moved = session && !threads.empty() && api.set_threads &&
                           api.set_threads(session, threads.data(), threads.size()) == 0;
        if (!moved) {
          if (session) api.close_session(session);
          session = nullptr;
          if (!threads.empty()) {
            std::vector<int32_t> ids(threads.begin(), threads.end());
            target_ns = target_work();
            session = api.create_session(manager, ids.data(), ids.size(), target_ns);
          }
        }
      }
      headroom = thermal ? api.thermal_headroom(thermal, 10) : __builtin_nanf("");  // 10 s ahead
      gpu_temperature = GpuTemperature();
      bool hot = false, cool = false;
      if (headroom == headroom) {
        hot = headroom >= 0.95f;
        cool = headroom <= 0.75f;
      } else if (gpu_temperature == gpu_temperature) {
        hot = gpu_temperature >= REXCVAR_GET(android_thermal_guard_hot_c);
        cool = gpu_temperature <= REXCVAR_GET(android_thermal_guard_cool_c);
      }
      {
        if (REXCVAR_GET(android_thermal_guard) && (hot || cool || guard_saved_scale)) {
          if (!guard_saved_scale && hot) {
            guard_saved_scale = REXCVAR_GET(gta4_draw_distance_scale);
            const double lowered = std::max(0.5, guard_saved_scale * 0.7);
            rex::cvar::SetFlagByName("gta4_draw_distance_scale", std::to_string(lowered));
            __android_log_print(ANDROID_LOG_WARN, "LibertyRecomp",
                                "thermal-guard: headroom %.2f gpu %.1fC, draw distance %.2f -> %.2f",
                                headroom, gpu_temperature, guard_saved_scale, lowered);
          } else if (guard_saved_scale && cool) {
            rex::cvar::SetFlagByName("gta4_draw_distance_scale", std::to_string(guard_saved_scale));
            __android_log_print(ANDROID_LOG_WARN, "LibertyRecomp",
                                "thermal-guard: headroom %.2f gpu %.1fC, draw distance restored to %.2f",
                                headroom, gpu_temperature, guard_saved_scale);
            guard_saved_scale = 0.0;
          }
        } else if (guard_saved_scale && !REXCVAR_GET(android_thermal_guard)) {
          rex::cvar::SetFlagByName("gta4_draw_distance_scale", std::to_string(guard_saved_scale));
          guard_saved_scale = 0.0;
        }
      }
    }

    if (session) {
      const int64_t wanted = target_work();
      if (wanted != target_ns) {
        target_ns = wanted;
        api.update_target(session, target_ns);
      }
      // The frame's critical path is the busiest of the session's threads.
      int64_t work = 0;
      for (size_t i = 0; i < threads.size(); ++i) {
        const int64_t time = ThreadCpuNanoseconds(threads[i]);
        if (time >= 0 && thread_times[i] >= 0) work = std::max(work, time - thread_times[i]);
        thread_times[i] = time;
      }
      if (work > 0) {
        api.report_actual(session, work);
        reported_sum_ms += double(work) / 1e6;
        ++reported;
      }
    }

    if (now >= next_log) {
      next_log = now + std::chrono::seconds(10);
      std::string ids;
      for (pid_t tid : threads) ids += (ids.empty() ? "" : ",") + std::to_string(tid);
      __android_log_print(ANDROID_LOG_WARN, "LibertyRecomp",
                          "power: headroom=%.2f gpu=%.1fC hint=%s work=%.1fms/target %.1fms "
                          "threads=%s%s",
                          headroom, gpu_temperature, session ? "on" : "off",
                          reported ? reported_sum_ms / reported : 0.0, double(target_ns) / 1e6,
                          ids.c_str(), guard_saved_scale ? " guard=lowered" : "");
      reported_sum_ms = 0.0;
      reported = 0;
    }
  }
}

const bool g_power_started = [] {
  std::thread(PowerLoop).detach();
  return true;
}();

}  // namespace
