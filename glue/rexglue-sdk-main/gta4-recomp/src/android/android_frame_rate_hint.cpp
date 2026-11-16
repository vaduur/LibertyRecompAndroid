// Tells Android which frame rate the game presents at.
//
// Phones with adaptive panels (Samsung "Adaptive" motion smoothness, LTPO
// panels in general) pick the refresh rate from what the visible windows ask
// for. A window that asks for nothing gets whatever the system's heuristics
// choose, and they change their mind: a 30 FPS game on a panel that moves
// between 60, 96 and 120 Hz is shown for an uneven number of refreshes per
// frame, and that judder reads as an unstable frame rate even when every
// frame was ready in time.
//
// Every second this reads gta4_frame_limit (the in-game Display menu changes
// it live) and, when it changed, hands it to
// LibertyActivity.setGameFrameRate(float). The Java side applies it with
// Surface.setFrameRate(fps, FRAME_RATE_COMPATIBILITY_FIXED_SOURCE) (API 30+)
// and re-applies it whenever SDL's surface is recreated. The system then
// prefers a refresh rate that is a multiple of the cap (60 or 120 Hz for 30
// FPS). An unlocked frame rate (0) clears the request.
#include <jni.h>

#include <chrono>
#include <cstdint>
#include <thread>

#include <SDL3/SDL_init.h>
#include <SDL3/SDL_system.h>
#include <android/log.h>

#include <rex/cvar.h>

REXCVAR_DEFINE_BOOL(android_frame_rate_hint, true, "GTA IV/Performance",
                    "Ask Android for a display refresh rate that is a multiple of the frame limit "
                    "(Surface.setFrameRate), so adaptive 90/120 Hz panels show every frame for the "
                    "same number of refreshes");

REXCVAR_DECLARE(uint32_t, gta4_frame_limit);

namespace {

enum class SendResult { kSent, kNotReady, kUnsupported };

SendResult SendFrameRate(float fps) {
  // SDL_GetAndroidActivity calls through SDL's cached SDLActivity class,
  // which is only set once SDLActivity has run SDL.setupJNI(); video is
  // initialized after that, so it marks the point where the call is safe.
  if (!SDL_WasInit(SDL_INIT_VIDEO)) return SendResult::kNotReady;
  JNIEnv* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
  if (!env) return SendResult::kNotReady;
  // Null until SDL has started; the loop retries.
  jobject activity = static_cast<jobject>(SDL_GetAndroidActivity());
  if (!activity) return SendResult::kNotReady;
  SendResult result = SendResult::kUnsupported;
  // GetObjectClass instead of FindClass: on a native thread FindClass only
  // sees the system class loader, not the app's classes.
  jclass activity_class = env->GetObjectClass(activity);
  jmethodID method =
      activity_class ? env->GetMethodID(activity_class, "setGameFrameRate", "(F)V") : nullptr;
  if (env->ExceptionCheck()) env->ExceptionClear();  // NoSuchMethodError: an older Java side
  if (method) {
    env->CallVoidMethod(activity, method, jfloat(fps));
    if (env->ExceptionCheck()) {
      env->ExceptionDescribe();
      env->ExceptionClear();
    } else {
      result = SendResult::kSent;
    }
  }
  // This thread stays attached for the process lifetime, so local
  // references are never released for it: drop them explicitly.
  if (activity_class) env->DeleteLocalRef(activity_class);
  env->DeleteLocalRef(activity);
  return result;
}

void FrameRateHintLoop() {
  float sent_fps = -1.0f;
  for (;;) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    const float fps =
        REXCVAR_GET(android_frame_rate_hint) ? float(REXCVAR_GET(gta4_frame_limit)) : 0.0f;
    if (fps == sent_fps) continue;
    switch (SendFrameRate(fps)) {
      case SendResult::kSent:
        __android_log_print(ANDROID_LOG_WARN, "LibertyRecomp", "frame-rate-hint: %.0f FPS%s",
                            fps, fps > 0.0f ? "" : " (no preference)");
        sent_fps = fps;
        break;
      case SendResult::kNotReady:
        break;
      case SendResult::kUnsupported:
        __android_log_print(ANDROID_LOG_WARN, "LibertyRecomp",
                            "frame-rate-hint: LibertyActivity.setGameFrameRate unavailable");
        return;
    }
  }
}

const bool g_frame_rate_hint_started = [] {
  std::thread(FrameRateHintLoop).detach();
  return true;
}();

}  // namespace
