#include "android/android_performance_hint.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <dirent.h>
#include <dlfcn.h>
#include <time.h>
#include <unistd.h>

#include <android/log.h>

#include <rex/cvar.h>

REXCVAR_DEFINE_BOOL(android_performance_hint, true, "GTA IV/Performance",
                    "Report frame times to Android's Performance Hint API so the system "
                    "raises the CPU clocks of the game's busiest threads when frames run late");

namespace gta4::android {
namespace {

constexpr int64_t kTargetFrameNs = 33'333'333;  // 30 FPS
constexpr size_t kMaxHintThreads = 4;
// CPU ticks (100 per second) a thread needs in one second to count as busy.
constexpr uint64_t kBusyTicks = 20;

std::mutex g_frames_mutex;
std::vector<int64_t> g_frame_durations;  // filled by NoteTitleFrame
int64_t g_last_frame_ns = 0;

int64_t NowNs() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return int64_t(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
}

// NDK API (libandroid.so, API 33+; setThreads is API 34+), loaded at runtime
// because the app's minSdk is lower.
struct HintApi {
  using GetManagerFn = void* (*)();
  using CreateSessionFn = void* (*)(void*, const int32_t*, size_t, int64_t);
  using ReportFn = int (*)(void*, int64_t);
  using SetThreadsFn = int (*)(void*, const pid_t*, size_t);
  using CloseFn = void (*)(void*);
  GetManagerFn get_manager = nullptr;
  CreateSessionFn create_session = nullptr;
  ReportFn report = nullptr;
  SetThreadsFn set_threads = nullptr;
  CloseFn close = nullptr;

  bool Load() {
    void* library = dlopen("libandroid.so", RTLD_NOW);
    if (!library) return false;
    get_manager = reinterpret_cast<GetManagerFn>(dlsym(library, "APerformanceHint_getManager"));
    create_session =
        reinterpret_cast<CreateSessionFn>(dlsym(library, "APerformanceHint_createSession"));
    report =
        reinterpret_cast<ReportFn>(dlsym(library, "APerformanceHint_reportActualWorkDuration"));
    set_threads = reinterpret_cast<SetThreadsFn>(dlsym(library, "APerformanceHint_setThreads"));
    close = reinterpret_cast<CloseFn>(dlsym(library, "APerformanceHint_closeSession"));
    return get_manager && create_session && report && close;
  }
};

// The busiest threads of this process over the last second, busiest first.
class BusyThreads {
 public:
  std::vector<pid_t> Sample(pid_t exclude) {
    std::unordered_map<pid_t, uint64_t> ticks;
    std::vector<std::pair<uint64_t, pid_t>> busy;
    if (DIR* tasks = opendir("/proc/self/task")) {
      while (const dirent* entry = readdir(tasks)) {
        if (!std::isdigit(static_cast<unsigned char>(entry->d_name[0]))) continue;
        const pid_t tid = pid_t(std::atoi(entry->d_name));
        if (tid == exclude) continue;
        std::ifstream stat_file(std::string("/proc/self/task/") + entry->d_name + "/stat");
        std::string stat((std::istreambuf_iterator<char>(stat_file)), {});
        const size_t name_end = stat.rfind(')');
        if (name_end == std::string::npos) continue;
        // Fields after the name start at 3 (state); utime is 14, stime 15.
        std::istringstream fields(stat.substr(name_end + 2));
        std::string field;
        uint64_t utime = 0, stime = 0;
        for (int index = 3; index <= 15 && fields >> field; ++index) {
          if (index == 14) utime = std::strtoull(field.c_str(), nullptr, 10);
          if (index == 15) stime = std::strtoull(field.c_str(), nullptr, 10);
        }
        ticks[tid] = utime + stime;
        const auto last = last_ticks_.find(tid);
        if (last != last_ticks_.end() && utime + stime - last->second >= kBusyTicks) {
          busy.emplace_back(utime + stime - last->second, tid);
        }
      }
      closedir(tasks);
    }
    last_ticks_ = std::move(ticks);
    std::sort(busy.begin(), busy.end(), std::greater<>());
    std::vector<pid_t> result;
    for (const auto& [delta, tid] : busy) {
      if (result.size() == kMaxHintThreads) break;
      result.push_back(tid);
    }
    std::sort(result.begin(), result.end());
    return result;
  }

 private:
  std::unordered_map<pid_t, uint64_t> last_ticks_;
};

void Log(const char* message, size_t count) {
  __android_log_print(ANDROID_LOG_WARN, "LibertyRecomp", "performance-hint: %s (%zu threads)",
                      message, count);
}

}  // namespace

void NoteTitleFrame() noexcept {
  const int64_t now = NowNs();
  std::lock_guard lock(g_frames_mutex);
  if (g_last_frame_ns != 0) {
    // Clamp pauses and loading stalls so one long gap does not dominate.
    g_frame_durations.push_back(std::min<int64_t>(now - g_last_frame_ns, 4 * kTargetFrameNs));
    if (g_frame_durations.size() > 256) g_frame_durations.erase(g_frame_durations.begin());
  }
  g_last_frame_ns = now;
}

void StartPerformanceHint() {
  std::thread([] {
    HintApi api;
    if (!api.Load()) {
      Log("not available on this Android version", 0);
      return;
    }
    void* manager = api.get_manager();
    if (!manager) {
      Log("no hint manager", 0);
      return;
    }
    const pid_t self = pid_t(gettid());
    BusyThreads busy;
    std::vector<pid_t> threads;
    void* session = nullptr;
    int tick = 0;
    for (;;) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      if (!REXCVAR_GET(android_performance_hint)) {
        if (session) {
          api.close(session);
          session = nullptr;
          threads.clear();
          Log("turned off", 0);
        }
        continue;
      }
      // Once a second: follow the busiest threads.
      if (++tick % 10 == 0) {
        std::vector<pid_t> hot = busy.Sample(self);
        if (!hot.empty() && hot != threads) {
          if (session && api.set_threads &&
              api.set_threads(session, hot.data(), hot.size()) == 0) {
            threads = hot;
          } else {
            if (session) api.close(session);
            session = api.create_session(manager, reinterpret_cast<const int32_t*>(hot.data()),
                                         hot.size(), kTargetFrameNs);
            threads = session ? hot : std::vector<pid_t>{};
            Log(session ? "session started" : "could not create a session", hot.size());
          }
        }
      }
      // Report the frames finished since the last pass.
      std::vector<int64_t> durations;
      {
        std::lock_guard lock(g_frames_mutex);
        durations.swap(g_frame_durations);
      }
      if (session) {
        for (int64_t duration : durations) {
          if (duration > 0) api.report(session, duration);
        }
      }
    }
  }).detach();
}

}  // namespace gta4::android
