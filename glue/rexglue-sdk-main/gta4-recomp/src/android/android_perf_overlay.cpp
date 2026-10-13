#include "android/android_perf_overlay.h"

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include <unistd.h>

#include <imgui.h>

#include <rex/cvar.h>

REXCVAR_DEFINE_BOOL(gta4_overlay_fps, false, "GTA IV/Overlay",
                    "Overlay: the game's real update rate (world frames per second)");
REXCVAR_DEFINE_BOOL(gta4_overlay_frametime, false, "GTA IV/Overlay",
                    "Overlay: game frame time in milliseconds");
REXCVAR_DEFINE_BOOL(gta4_overlay_cpu, false, "GTA IV/Overlay",
                    "Overlay: the game's CPU usage and the fastest core's clock");
REXCVAR_DEFINE_BOOL(gta4_overlay_gpu, false, "GTA IV/Overlay",
                    "Overlay: GPU usage, where the system exposes it");
REXCVAR_DEFINE_BOOL(gta4_overlay_memory, false, "GTA IV/Overlay",
                    "Overlay: the game's memory use");
REXCVAR_DEFINE_BOOL(gta4_overlay_temperature, false, "GTA IV/Overlay",
                    "Overlay: battery temperature (a stand-in for how hot the phone is)");

// Measured in the title's frame-wait hook (android_frame_wait_hook.cpp).
extern std::atomic<float> g_liberty_title_fps;

namespace gta4::android {
namespace {

double Now() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return double(ts.tv_sec) + double(ts.tv_nsec) / 1e9;
}

bool ReadFile(const char* path, char* buffer, size_t size) {
  FILE* file = std::fopen(path, "r");
  if (!file) return false;
  const size_t read = std::fread(buffer, 1, size - 1, file);
  std::fclose(file);
  buffer[read] = 0;
  return read > 0;
}

// Total user+system CPU time of this process, in seconds.
bool ProcessCpuSeconds(double* seconds) {
  char buffer[1024];
  if (!ReadFile("/proc/self/stat", buffer, sizeof(buffer))) return false;
  // Fields after the ")" that ends the process name: state is field 3,
  // utime is 14 and stime is 15.
  const char* p = std::strrchr(buffer, ')');
  if (!p) return false;
  unsigned long long utime = 0, stime = 0;
  if (std::sscanf(p + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &utime,
                  &stime) != 2) {
    return false;
  }
  const long ticks = sysconf(_SC_CLK_TCK);
  *seconds = double(utime + stime) / double(ticks > 0 ? ticks : 100);
  return true;
}

// Highest current clock across all cores, in MHz (0 if unreadable).
int FastestCoreMhz() {
  int best = 0;
  const long cores = sysconf(_SC_NPROCESSORS_CONF);
  for (long i = 0; i < cores && i < 16; ++i) {
    char path[96], buffer[32];
    std::snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%ld/cpufreq/scaling_cur_freq",
                  i);
    if (ReadFile(path, buffer, sizeof(buffer))) {
      const int mhz = std::atoi(buffer) / 1000;
      if (mhz > best) best = mhz;
    }
  }
  return best;
}

// GPU busy percentage from the vendor driver's sysfs nodes, or -1 when the
// system does not let apps read any of them.
int GpuUsagePercent() {
  static const char* const kPaths[] = {
      "/sys/kernel/ged/hal/gpu_utilization",       // MediaTek GED: "<busy> <...>"
      "/sys/module/ged/parameters/gpu_loading",    // MediaTek GED (older)
      "/sys/class/misc/mali0/device/utilization",  // Mali kbase on some kernels
      "/sys/kernel/gpu/gpu_busy",                  // Generic (some vendors)
      "/sys/class/kgsl/kgsl-3d0/gpu_busy_percentage",  // Adreno
  };
  static int working = -2;  // -2: not probed yet, -1: none readable
  char buffer[64];
  if (working >= 0) {
    if (ReadFile(kPaths[working], buffer, sizeof(buffer))) return std::atoi(buffer);
    working = -2;
  }
  if (working == -2) {
    working = -1;
    for (int i = 0; i < int(sizeof(kPaths) / sizeof(kPaths[0])); ++i) {
      if (ReadFile(kPaths[i], buffer, sizeof(buffer))) {
        const int value = std::atoi(buffer);
        if (value >= 0 && value <= 100) {
          working = i;
          return value;
        }
      }
    }
  }
  return -1;
}

double ResidentMegabytes() {
  char buffer[128];
  if (!ReadFile("/proc/self/statm", buffer, sizeof(buffer))) return -1.0;
  unsigned long long size = 0, resident = 0;
  if (std::sscanf(buffer, "%llu %llu", &size, &resident) != 2) return -1.0;
  return double(resident) * double(sysconf(_SC_PAGESIZE)) / (1024.0 * 1024.0);
}

double BatteryCelsius() {
  char buffer[32];
  if (!ReadFile("/sys/class/power_supply/battery/temp", buffer, sizeof(buffer))) return -1000.0;
  return std::atoi(buffer) / 10.0;
}

struct Sample {
  double time = 0.0;
  double cpu_seconds = 0.0;
  int cpu_percent = -1;
  int cpu_mhz = 0;
  int gpu_percent = -1;
  double memory_mb = -1.0;
  double battery_c = -1000.0;
};

Sample g_sample;

// Reads the system counters at most twice a second.
void Refresh() {
  const double now = Now();
  if (g_sample.time != 0.0 && now - g_sample.time < 0.5) return;
  if (REXCVAR_GET(gta4_overlay_cpu)) {
    double cpu_seconds = 0.0;
    if (ProcessCpuSeconds(&cpu_seconds)) {
      if (g_sample.time != 0.0 && g_sample.cpu_seconds > 0.0) {
        const long cores = sysconf(_SC_NPROCESSORS_ONLN);
        const double share = (cpu_seconds - g_sample.cpu_seconds) / (now - g_sample.time) /
                             double(cores > 0 ? cores : 1);
        g_sample.cpu_percent = int(share * 100.0 + 0.5);
      }
      g_sample.cpu_seconds = cpu_seconds;
    }
    g_sample.cpu_mhz = FastestCoreMhz();
  }
  if (REXCVAR_GET(gta4_overlay_gpu)) g_sample.gpu_percent = GpuUsagePercent();
  if (REXCVAR_GET(gta4_overlay_memory)) g_sample.memory_mb = ResidentMegabytes();
  if (REXCVAR_GET(gta4_overlay_temperature)) g_sample.battery_c = BatteryCelsius();
  g_sample.time = now;
}

}  // namespace

bool PerfOverlayEnabled() noexcept {
  return REXCVAR_GET(gta4_overlay_fps) || REXCVAR_GET(gta4_overlay_frametime) ||
         REXCVAR_GET(gta4_overlay_cpu) || REXCVAR_GET(gta4_overlay_gpu) ||
         REXCVAR_GET(gta4_overlay_memory) || REXCVAR_GET(gta4_overlay_temperature);
}

void DrawPerfOverlay(ImDrawList* draw, ImFont* font, float width, float height) noexcept {
  if (!draw || !font || width <= 0.0f || height <= 0.0f || !PerfOverlayEnabled()) return;
  Refresh();
  const float fps = g_liberty_title_fps.load(std::memory_order_relaxed);

  struct Line {
    char text[48];
    ImU32 color;
  };
  std::vector<Line> lines;
  auto add = [&lines](ImU32 color, const char* format, auto... values) {
    Line line{};
    std::snprintf(line.text, sizeof(line.text), format, values...);
    line.color = color;
    lines.push_back(line);
  };
  // Green above 28, yellow above 20, red below.
  const ImU32 fps_color = fps >= 28.0f   ? IM_COL32(120, 230, 120, 255)
                          : fps >= 20.0f ? IM_COL32(240, 210, 90, 255)
                                         : IM_COL32(240, 100, 100, 255);
  const ImU32 white = IM_COL32(235, 235, 235, 255);
  if (REXCVAR_GET(gta4_overlay_fps)) add(fps_color, "FPS  %.0f", fps);
  if (REXCVAR_GET(gta4_overlay_frametime)) {
    if (fps > 0.5f) add(white, "MS   %.1f", 1000.0f / fps);
    else add(white, "%s", "MS   --");
  }
  if (REXCVAR_GET(gta4_overlay_cpu)) {
    if (g_sample.cpu_percent >= 0 && g_sample.cpu_mhz > 0)
      add(white, "CPU  %d%%  %.1fGHz", g_sample.cpu_percent, g_sample.cpu_mhz / 1000.0);
    else if (g_sample.cpu_percent >= 0) add(white, "CPU  %d%%", g_sample.cpu_percent);
    else add(white, "%s", "CPU  --");
  }
  if (REXCVAR_GET(gta4_overlay_gpu)) {
    if (g_sample.gpu_percent >= 0) add(white, "GPU  %d%%", g_sample.gpu_percent);
    else add(white, "%s", "GPU  n/a");
  }
  if (REXCVAR_GET(gta4_overlay_memory)) {
    if (g_sample.memory_mb >= 0.0) add(white, "MEM  %.0fMB", g_sample.memory_mb);
    else add(white, "%s", "MEM  --");
  }
  if (REXCVAR_GET(gta4_overlay_temperature)) {
    if (g_sample.battery_c > -100.0) add(white, "TEMP %.0fC", g_sample.battery_c);
    else add(white, "%s", "TEMP --");
  }
  if (lines.empty()) return;

  // Small: each line is 2.6% of the screen height (about 19 px at 720p).
  const float size = height * 0.026f;
  const float pad = size * 0.35f;
  const float line_gap = size * 1.12f;
  float text_width = 0.0f;
  for (const Line& line : lines) {
    text_width = std::max(text_width, font->CalcTextSizeA(size, FLT_MAX, 0.0f, line.text).x);
  }
  const ImVec2 min(height * 0.015f, height * 0.015f);
  const ImVec2 max(min.x + text_width + pad * 2.0f,
                   min.y + line_gap * float(lines.size()) + pad * 2.0f - (line_gap - size));
  draw->AddRectFilled(min, max, IM_COL32(0, 0, 0, 140), pad);
  float y = min.y + pad;
  for (const Line& line : lines) {
    draw->AddText(font, size, ImVec2(min.x + pad, y), line.color, line.text);
    y += line_gap;
  }
}

}  // namespace gta4::android
