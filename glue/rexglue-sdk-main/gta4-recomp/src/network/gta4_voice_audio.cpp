#include "network/gta4_voice_audio.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <SDL3/SDL.h>

#include <rex/logging.h>
#include <rex/thread/jthread_compat.h>
#include <rex/runtime.h>
#include <rex/system/xam/live_compatibility.h>
#include <rex/system/xmemory.h>

#include "gta4_init.h"
#include "network/gta4_microphone_permission.h"

namespace gta4::voice {
namespace {

constexpr int kVoiceSampleRate = 16000;
constexpr int kVoiceChannelCount = 1;
constexpr uint32_t kCodecAllocationBytes = 192;
constexpr uint32_t kCodecAllocationAlignment = 32;
constexpr uint32_t kCodecOutputOffset = 64;
constexpr uint32_t kCodecStateSeedAddress = 0x820AB908;
constexpr uint32_t kCodecStateBytes = 54;
constexpr uint8_t kCodecNibbleMask = 0x0F;
constexpr auto kLifecycleRetryInterval = std::chrono::seconds(1);

class SDLVoiceAudioDevice final : public rex::system::xam::IVoiceAudioDevice {
 public:
  SDLVoiceAudioDevice()
      : lifecycle_signal_(std::make_shared<LifecycleSignal>()),
        lifecycle_worker_([this](rex::compat::stop_token stop_token) { LifecycleWorkerMain(stop_token); }) {
  }

  ~SDLVoiceAudioDevice() override {
    lifecycle_worker_.request_stop();
    SignalLifecycle(lifecycle_signal_);
    if (lifecycle_worker_.joinable())
      lifecycle_worker_.join();
  }

  bool Open() override {
    {
      std::lock_guard lock(state_mutex_);
      if (open_references_ == std::numeric_limits<size_t>::max()) {
        subsystem_error_ = "voice audio reference count overflow";
        return false;
      }
      if (!open_references_) {
        subsystem_error_.clear();
        playback_error_.clear();
        capture_error_.clear();
      }
      ++open_references_;
    }
    // Retail creates the voice handle once and polls XamVoiceHeadsetPresent
    // for later changes. Keep that logical handle alive while the host
    // permission or physical devices are temporarily unavailable.
    SignalLifecycle(lifecycle_signal_);
    return true;
  }

  void Close() override {
    bool became_inactive = false;
    {
      std::lock_guard lock(state_mutex_);
      if (!open_references_)
        return;
      --open_references_;
      became_inactive = !open_references_;
    }
    if (became_inactive)
      SignalLifecycle(lifecycle_signal_);
  }

  bool playback_available() const noexcept override {
    std::lock_guard lock(state_mutex_);
    return open_references_ && playback_stream_ && !playback_stream_invalidated_;
  }

  bool capture_available() const noexcept override {
    std::lock_guard lock(state_mutex_);
    return open_references_ && permission_granted_ && capture_stream_ &&
           !capture_stream_invalidated_;
  }

  size_t Capture(std::span<int16_t> samples, std::chrono::milliseconds timeout) override {
    if (samples.empty())
      return 0;
    const auto bytes = std::as_writable_bytes(samples);
    if (bytes.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
      return 0;

    std::unique_lock stream_lock(stream_mutex_);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    size_t received_bytes = 0;
    while (received_bytes < bytes.size()) {
      SDL_AudioStream* stream = nullptr;
      {
        std::lock_guard lock(state_mutex_);
        if (!open_references_ || !permission_granted_ || capture_stream_invalidated_) {
          break;
        }
        stream = capture_stream_;
      }
      if (!stream)
        break;

      const int available = SDL_GetAudioStreamAvailable(stream);
      if (available < 0) {
        InvalidateEndpoint(true, "capture query", SDL_GetError());
        break;
      }
      if (available > 0) {
        const size_t request =
            std::min(bytes.size() - received_bytes, static_cast<size_t>(available));
        const int received = SDL_GetAudioStreamData(stream, bytes.data() + received_bytes,
                                                    static_cast<int>(request));
        if (received < 0) {
          InvalidateEndpoint(true, "capture read", SDL_GetError());
          break;
        }
        if (received > 0) {
          received_bytes += static_cast<size_t>(received);
          continue;
        }
      }
      if (timeout <= std::chrono::milliseconds::zero() ||
          capture_condition_.wait_until(stream_lock, deadline) == std::cv_status::timeout) {
        break;
      }
    }
    bool log_capture = false;
    if (received_bytes) {
      std::lock_guard lock(state_mutex_);
      log_capture = !logged_capture_;
      logged_capture_ = true;
    }
    if (log_capture) {
      REXSYS_INFO("GTA IV voice microphone produced nonempty PCM capture");
    }
    return received_bytes / sizeof(int16_t);
  }

  bool Play(std::span<const int16_t> samples) override {
    if (samples.empty())
      return true;
    const auto bytes = std::as_bytes(samples);
    if (bytes.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
      return false;

    std::lock_guard stream_lock(stream_mutex_);
    SDL_AudioStream* stream = nullptr;
    {
      std::lock_guard lock(state_mutex_);
      if (!open_references_ || playback_stream_invalidated_)
        return false;
      stream = playback_stream_;
    }
    if (!stream)
      return false;
    if (!SDL_PutAudioStreamData(stream, bytes.data(), static_cast<int>(bytes.size()))) {
      InvalidateEndpoint(false, "playback write", SDL_GetError());
      return false;
    }
    bool log_playback = false;
    {
      std::lock_guard lock(state_mutex_);
      log_playback = !logged_playback_;
      logged_playback_ = true;
    }
    if (log_playback) {
      REXSYS_INFO("GTA IV voice decoded PCM reached the SDL playback device");
    }
    return true;
  }

  std::string last_error() const override {
    std::lock_guard lock(state_mutex_);
    std::string result;
    AppendError(result, subsystem_error_);
    AppendError(result, playback_error_);
    AppendError(result, capture_error_);
    return result;
  }

 private:
  struct LifecycleSignal {
    std::mutex mutex;
    std::condition_variable condition;
    bool requested = false;
    bool permission_completed = false;
  };

  static void SDLCALL OnCaptureData(void* userdata, SDL_AudioStream*, int, int) {
    static_cast<SDLVoiceAudioDevice*>(userdata)->capture_condition_.notify_all();
  }

  static bool SDLCALL OnSDLEvent(void* userdata, SDL_Event* event) {
    auto* self = static_cast<SDLVoiceAudioDevice*>(userdata);
    if (event->type < SDL_EVENT_AUDIO_DEVICE_FIRST || event->type > SDL_EVENT_AUDIO_DEVICE_LAST) {
      return true;
    }

    const bool invalidate = event->type == SDL_EVENT_AUDIO_DEVICE_REMOVED;
    if (invalidate) {
      {
        std::lock_guard lock(self->state_mutex_);
        if (event->adevice.recording) {
          self->capture_stream_invalidated_ = true;
          self->capture_error_ = "capture device was disconnected";
        } else {
          self->playback_stream_invalidated_ = true;
          self->playback_error_ = "playback device was disconnected";
        }
      }
      if (event->adevice.recording)
        self->capture_condition_.notify_all();
    }
    SignalLifecycle(self->lifecycle_signal_);
    return true;
  }

  static void AppendError(std::string& destination, std::string_view error) {
    if (error.empty())
      return;
    if (!destination.empty())
      destination += "; ";
    destination.append(error);
  }

  static std::string MakeError(std::string_view operation, std::string_view error) {
    std::string result(operation);
    result += ": ";
    result.append(error.empty() ? "unknown SDL audio error" : error);
    return result;
  }

  static std::string MakeError(std::string_view operation, const char* error) {
    return MakeError(operation, error ? std::string_view(error) : std::string_view());
  }

  static void SignalLifecycle(const std::shared_ptr<LifecycleSignal>& signal,
                              bool permission_completed = false) {
    {
      std::lock_guard lock(signal->mutex);
      signal->requested = true;
      signal->permission_completed |= permission_completed;
    }
    signal->condition.notify_all();
  }

  bool WaitForLifecycle(rex::compat::stop_token stop_token) {
    bool active = false;
    {
      std::lock_guard lock(state_mutex_);
      active = open_references_ != 0;
    }

    std::unique_lock lock(lifecycle_signal_->mutex);
    const auto ready = [&] { return stop_token.stop_requested() || lifecycle_signal_->requested; };
    if (active) {
      lifecycle_signal_->condition.wait_for(lock, kLifecycleRetryInterval, ready);
    } else {
      lifecycle_signal_->condition.wait(lock, ready);
    }
    const bool permission_completed = lifecycle_signal_->permission_completed;
    lifecycle_signal_->requested = false;
    lifecycle_signal_->permission_completed = false;
    if (permission_completed)
      permission_request_pending_ = false;
    return !stop_token.stop_requested();
  }

  void LifecycleWorkerMain(rex::compat::stop_token stop_token) {
    while (WaitForLifecycle(stop_token))
      ReconcileLifecycle();
    ShutdownAudio();
  }

  bool IsActive() const {
    std::lock_guard lock(state_mutex_);
    return open_references_ != 0;
  }

  bool EnsureAudioSubsystem() {
    if (audio_subsystem_ready_)
      return true;

    if (!(SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO)) {
      if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        std::lock_guard lock(state_mutex_);
        subsystem_error_ = MakeError("audio subsystem init", SDL_GetError());
        return false;
      }
      owns_audio_subsystem_ = true;
    }

    audio_subsystem_ready_ = true;
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_STREAM_ROLE, "GameChat");
    event_watch_installed_ = SDL_AddEventWatch(OnSDLEvent, this);
    if (!event_watch_installed_) {
      REXSYS_WARN("GTA IV voice audio event watch unavailable: {}", SDL_GetError());
    }
    {
      std::lock_guard lock(state_mutex_);
      subsystem_error_.clear();
    }
    return true;
  }

  static bool HasAudioDevices(bool recording, std::string& error) {
    int count = 0;
    SDL_AudioDeviceID* devices =
        recording ? SDL_GetAudioRecordingDevices(&count) : SDL_GetAudioPlaybackDevices(&count);
    const bool query_failed = devices == nullptr;
    if (devices)
      SDL_free(devices);
    if (count > 0)
      return true;
    if (query_failed)
      error = SDL_GetError();
    return false;
  }

  void UpdateEnvironment(MicrophonePermissionStatus permission, bool playback_device_available,
                         bool capture_device_available, std::string_view playback_device_error,
                         std::string_view capture_device_error) {
    std::lock_guard lock(state_mutex_);
    permission_granted_ = permission == MicrophonePermissionStatus::kAuthorized;
    playback_device_available_ = playback_device_available;
    capture_device_available_ = capture_device_available;

    if (!playback_device_available_) {
      playback_stream_invalidated_ = true;
      playback_error_ = playback_device_error.empty()
                            ? "no playback device is available"
                            : MakeError("playback device query", playback_device_error);
    }

    if (!capture_device_available_)
      capture_stream_invalidated_ = true;
    switch (permission) {
      case MicrophonePermissionStatus::kNotDetermined:
        capture_stream_invalidated_ = true;
        capture_error_ = "waiting for macOS microphone authorization";
        break;
      case MicrophonePermissionStatus::kDenied:
        capture_stream_invalidated_ = true;
        capture_error_ = "microphone access was denied in macOS Privacy settings";
        break;
      case MicrophonePermissionStatus::kRestricted:
        capture_stream_invalidated_ = true;
        capture_error_ = "microphone access is restricted by macOS policy";
        break;
      case MicrophonePermissionStatus::kUnknown:
        capture_stream_invalidated_ = true;
        capture_error_ = "macOS returned an unknown microphone authorization state";
        break;
      case MicrophonePermissionStatus::kAuthorized:
        if (!capture_device_available_) {
          capture_error_ = capture_device_error.empty()
                               ? "no recording device is available"
                               : MakeError("capture device query", capture_device_error);
        }
        break;
    }
  }

  detail::VoiceDeviceLifecycleInputs LifecycleInputs() const {
    std::lock_guard lock(state_mutex_);
    return {
        .active = open_references_ != 0,
        .permission_granted = permission_granted_,
        .playback_device_available = playback_device_available_,
        .capture_device_available = capture_device_available_,
        .playback_stream_open = playback_stream_ != nullptr,
        .capture_stream_open = capture_stream_ != nullptr,
        .playback_stream_invalidated = playback_stream_invalidated_,
        .capture_stream_invalidated = capture_stream_invalidated_,
    };
  }

  void ReconcileLifecycle() {
    if (!IsActive()) {
      ShutdownAudio();
      permission_request_pending_ = false;
      return;
    }

    const MicrophonePermissionStatus permission = GetMicrophonePermissionStatus();
    if (permission == MicrophonePermissionStatus::kNotDetermined && !permission_request_pending_) {
      permission_request_pending_ = true;
      const std::weak_ptr<LifecycleSignal> weak_signal = lifecycle_signal_;
      RequestMicrophonePermission([weak_signal] {
        if (const auto signal = weak_signal.lock()) {
          SignalLifecycle(signal, true);
        }
      });
    } else if (permission != MicrophonePermissionStatus::kNotDetermined) {
      permission_request_pending_ = false;
    }

    if (!EnsureAudioSubsystem()) {
      UpdateEnvironment(permission, false, false, {}, {});
      CloseEndpoint(false);
      CloseEndpoint(true);
      return;
    }

    std::string playback_device_error;
    std::string capture_device_error;
    const bool playback_device_available = HasAudioDevices(false, playback_device_error);
    const bool capture_device_available = HasAudioDevices(true, capture_device_error);
    UpdateEnvironment(permission, playback_device_available, capture_device_available,
                      playback_device_error, capture_device_error);

    const auto actions = detail::EvaluateVoiceDeviceLifecycle(LifecycleInputs());
    if (actions.close_playback)
      CloseEndpoint(false);
    if (actions.close_capture)
      CloseEndpoint(true);
    if (actions.open_playback)
      OpenEndpoint(false);
    if (actions.open_capture)
      OpenEndpoint(true);
  }

  bool EndpointWantedLocked(bool recording) const {
    if (!open_references_)
      return false;
    if (recording) {
      return permission_granted_ && capture_device_available_ && !capture_stream_;
    }
    return playback_device_available_ && !playback_stream_;
  }

  void OpenEndpoint(bool recording) {
    {
      std::lock_guard lock(state_mutex_);
      if (!EndpointWantedLocked(recording))
        return;
      if (recording) {
        capture_stream_invalidated_ = false;
      } else {
        playback_stream_invalidated_ = false;
      }
    }

    const SDL_AudioSpec spec{SDL_AUDIO_S16, kVoiceChannelCount, kVoiceSampleRate};
    SDL_AudioStream* candidate = SDL_OpenAudioDeviceStream(
        recording ? SDL_AUDIO_DEVICE_DEFAULT_RECORDING : SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec,
        recording ? &OnCaptureData : nullptr, recording ? this : nullptr);
    if (!candidate) {
      SetEndpointError(recording, recording ? "capture open" : "playback open", SDL_GetError());
      return;
    }
    if (!SDL_ResumeAudioStreamDevice(candidate)) {
      const std::string error =
          MakeError(recording ? "capture resume" : "playback resume", SDL_GetError());
      SDL_DestroyAudioStream(candidate);
      SetEndpointError(recording, error);
      return;
    }

    bool published = false;
    {
      std::lock_guard lock(state_mutex_);
      const bool invalidated =
          recording ? capture_stream_invalidated_ : playback_stream_invalidated_;
      if (EndpointWantedLocked(recording) && !invalidated) {
        if (recording) {
          capture_stream_ = candidate;
          capture_stream_invalidated_ = false;
          capture_error_.clear();
        } else {
          playback_stream_ = candidate;
          playback_stream_invalidated_ = false;
          playback_error_.clear();
        }
        published = true;
      }
    }
    if (!published) {
      SDL_DestroyAudioStream(candidate);
      return;
    }

    REXSYS_INFO("GTA IV voice {} device opened at {} Hz, mono S16",
                recording ? "capture" : "playback", kVoiceSampleRate);
  }

  void CloseEndpoint(bool recording) {
    SDL_AudioStream* stream = nullptr;
    {
      std::lock_guard lock(state_mutex_);
      if (recording) {
        stream = std::exchange(capture_stream_, nullptr);
        capture_stream_invalidated_ = false;
      } else {
        stream = std::exchange(playback_stream_, nullptr);
        playback_stream_invalidated_ = false;
      }
    }
    if (!stream)
      return;

    if (recording)
      capture_condition_.notify_all();
    std::lock_guard stream_lock(stream_mutex_);
    SDL_DestroyAudioStream(stream);
    REXSYS_INFO("GTA IV voice {} device closed", recording ? "capture" : "playback");
  }

  void ShutdownAudio() {
    CloseEndpoint(true);
    CloseEndpoint(false);
    if (event_watch_installed_) {
      SDL_RemoveEventWatch(OnSDLEvent, this);
      event_watch_installed_ = false;
    }
    if (owns_audio_subsystem_) {
      SDL_QuitSubSystem(SDL_INIT_AUDIO);
      owns_audio_subsystem_ = false;
    }
    audio_subsystem_ready_ = false;
    {
      std::lock_guard lock(state_mutex_);
      playback_device_available_ = false;
      capture_device_available_ = false;
      permission_granted_ = false;
    }
  }

  void SetEndpointError(bool recording, std::string_view operation, const char* error) {
    SetEndpointError(recording, MakeError(operation, error));
  }

  void SetEndpointError(bool recording, std::string error) {
    std::lock_guard lock(state_mutex_);
    if (recording) {
      capture_error_ = std::move(error);
    } else {
      playback_error_ = std::move(error);
    }
  }

  void InvalidateEndpoint(bool recording, std::string_view operation, const char* error) {
    {
      std::lock_guard lock(state_mutex_);
      if (recording) {
        capture_stream_invalidated_ = true;
        capture_error_ = MakeError(operation, error);
      } else {
        playback_stream_invalidated_ = true;
        playback_error_ = MakeError(operation, error);
      }
    }
    if (recording)
      capture_condition_.notify_all();
    SignalLifecycle(lifecycle_signal_);
  }

  mutable std::mutex state_mutex_;
  std::mutex stream_mutex_;
  std::condition_variable capture_condition_;
  SDL_AudioStream* capture_stream_ = nullptr;
  SDL_AudioStream* playback_stream_ = nullptr;
  size_t open_references_ = 0;
  bool permission_granted_ = false;
  bool capture_device_available_ = false;
  bool playback_device_available_ = false;
  bool capture_stream_invalidated_ = false;
  bool playback_stream_invalidated_ = false;
  bool audio_subsystem_ready_ = false;
  bool owns_audio_subsystem_ = false;
  bool event_watch_installed_ = false;
  bool permission_request_pending_ = false;
  bool logged_capture_ = false;
  bool logged_playback_ = false;
  std::string subsystem_error_;
  std::string capture_error_;
  std::string playback_error_;
  std::shared_ptr<LifecycleSignal> lifecycle_signal_;
  rex::compat::jthread lifecycle_worker_;
};

class Gta4VoiceSampleCodecState final : public rex::system::xam::IVoiceSampleCodecState {
 public:
  Gta4VoiceSampleCodecState() {
    auto* runtime = rex::Runtime::instance();
    memory_ = runtime ? runtime->memory() : nullptr;
    if (!memory_)
      return;
    base_ = memory_->virtual_membase();
    allocation_ = memory_->SystemHeapAlloc(kCodecAllocationBytes, kCodecAllocationAlignment);
    if (allocation_) {
      std::memset(base_ + allocation_, 0, kCodecAllocationBytes);
      // sub_82A29D08 initializes every retail voice codec state by copying
      // this exact 54-byte title seed. A zero state produces a different
      // compressed stream and cannot interoperate with GTA's own talkers.
      std::memcpy(base_ + allocation_, base_ + kCodecStateSeedAddress, kCodecStateBytes);
    }
  }

  ~Gta4VoiceSampleCodecState() override {
    if (memory_ && allocation_)
      memory_->SystemHeapFree(allocation_);
  }

  bool valid() const { return memory_ && base_ && allocation_; }

  bool Encode(int16_t sample, uint8_t& encoded) override {
    uint16_t result = 0;
    if (!Run(false, static_cast<uint16_t>(sample), result) || result > kCodecNibbleMask) {
      return false;
    }
    encoded = static_cast<uint8_t>(result);
    return true;
  }

  bool Decode(uint8_t encoded, int16_t& sample) override {
    if (encoded > kCodecNibbleMask)
      return false;
    uint16_t result = 0;
    if (!Run(true, encoded, result))
      return false;
    sample = static_cast<int16_t>(result);
    return true;
  }

 private:
  bool Run(bool decode, uint16_t input, uint16_t& output) {
    if (!valid())
      return false;
    PPCContext ctx{};
    ctx.r1.u64 = allocation_ + kCodecAllocationBytes;
    ctx.r3.u64 = decode ? 1 : 0;
    ctx.r4.u64 = input;
    ctx.r5.u64 = allocation_ + kCodecOutputOffset;
    ctx.r6.u64 = allocation_;
    sub_82A2BBD8(ctx, base_);
    output = __builtin_bswap16(
        *reinterpret_cast<volatile uint16_t*>(base_ + allocation_ + kCodecOutputOffset));
    return true;
  }

  rex::memory::Memory* memory_ = nullptr;
  uint8_t* base_ = nullptr;
  uint32_t allocation_ = 0;
};

class Gta4VoiceSampleCodec final : public rex::system::xam::IVoiceSampleCodec {
 public:
  std::unique_ptr<rex::system::xam::IVoiceSampleCodecState> CreateState() override {
    auto state = std::make_unique<Gta4VoiceSampleCodecState>();
    if (!state->valid())
      return nullptr;
    return state;
  }
};

}  // namespace

std::shared_ptr<rex::system::xam::IVoiceAudioDevice> CreateAudioDevice() {
  return std::make_shared<SDLVoiceAudioDevice>();
}

std::shared_ptr<rex::system::xam::IVoiceSampleCodec> CreateSampleCodec() {
  return std::make_shared<Gta4VoiceSampleCodec>();
}

}  // namespace gta4::voice
