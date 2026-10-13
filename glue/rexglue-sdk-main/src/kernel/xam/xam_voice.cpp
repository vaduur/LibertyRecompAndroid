/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

// Disable warnings about unused parameters for kernel functions
#pragma GCC diagnostic ignored "-Wunused-parameter"

#include <rex/kernel/xam/private.h>
#include <rex/logging.h>
#include <rex/hook.h>
#include <rex/types.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xtypes.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <thread>

#include <rex/thread/jthread_compat.h>
#include <unordered_map>
#include <vector>

namespace rex {
namespace kernel {
namespace xam {

namespace {

constexpr uint32_t kVoicePacketHeaderSize = 24;
constexpr uint32_t kMaximumVoicePacketBytes = 4096;
constexpr uint32_t kVoicePacketPending = 259;
constexpr uint32_t kVoiceCodecTag = 1;
constexpr uint8_t kVoiceNibbleMask = 0x0F;
constexpr uint32_t kVoiceNibbleShift = 4;
constexpr size_t kSamplesPerEncodedByte = 2;
constexpr std::chrono::milliseconds kCaptureTimeout{20};

struct VoiceWorkItem {
  uint32_t packet_ptr = 0;
  uint32_t payload_ptr = 0;
  uint32_t payload_size = 0;
  std::vector<uint8_t> playback_payload;
};

bool IsGuestRangeValid(uint32_t address, size_t size);

struct VoiceHandleState {
  VoiceHandleState(
      std::shared_ptr<system::xam::IVoiceAudioDevice> audio_device,
      std::unique_ptr<system::xam::IVoiceSampleCodecState> capture_codec,
      std::unique_ptr<system::xam::IVoiceSampleCodecState> playback_codec)
      : audio_device(std::move(audio_device)),
        capture_codec(std::move(capture_codec)),
        playback_codec(std::move(playback_codec)) {}

  ~VoiceHandleState() { Stop(); }

  void Start() {
    capture_worker = rex::compat::jthread(
        [this](rex::compat::stop_token stop_token) { CaptureWorkerMain(stop_token); });
    playback_worker = rex::compat::jthread(
        [this](rex::compat::stop_token stop_token) { PlaybackWorkerMain(stop_token); });
  }

  bool EnqueueCapture(VoiceWorkItem item) {
    {
      std::lock_guard lock(mutex);
      if (stopping) return false;
      capture_queue.push_back(std::move(item));
    }
    condition.notify_all();
    return true;
  }

  bool EnqueuePlayback(VoiceWorkItem item) {
    {
      std::lock_guard lock(mutex);
      if (stopping) return false;
      playback_queue.push_back(std::move(item));
    }
    condition.notify_all();
    return true;
  }

  bool capture_available() const {
    return audio_device && audio_device->capture_available();
  }

  void Stop() {
    std::call_once(stop_once, [this] {
      std::deque<VoiceWorkItem> pending_capture;
      std::deque<VoiceWorkItem> pending_playback;
      {
        std::lock_guard lock(mutex);
        stopping = true;
        pending_capture.swap(capture_queue);
        pending_playback.swap(playback_queue);
      }
      capture_worker.request_stop();
      playback_worker.request_stop();
      condition.notify_all();
      if (capture_worker.joinable()) capture_worker.join();
      if (playback_worker.joinable()) playback_worker.join();

      for (const auto& item : pending_capture) CompleteError(item, false);
      for (const auto& item : pending_playback) CompleteError(item, true);
      if (audio_device) audio_device->Close();
    });
  }

  std::shared_ptr<system::xam::IVoiceAudioDevice> audio_device;
  std::unique_ptr<system::xam::IVoiceSampleCodecState> capture_codec;
  std::unique_ptr<system::xam::IVoiceSampleCodecState> playback_codec;

 private:
  static bool IsPacketValid(const VoiceWorkItem& item) {
    return IsGuestRangeValid(item.packet_ptr, kVoicePacketHeaderSize);
  }

  static void CompleteError(const VoiceWorkItem& item, bool playback) {
    if (!IsPacketValid(item)) return;
    auto* packet = REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(item.packet_ptr);
    memory::store_and_swap<uint32_t>(packet + 4, playback ? item.payload_size : 0);
    std::atomic_thread_fence(std::memory_order_release);
    memory::store_and_swap<uint32_t>(packet + 0,
                                     static_cast<uint32_t>(X_STATUS_UNSUCCESSFUL));
  }

  static void CompleteCapture(const VoiceWorkItem& item,
                              std::span<const uint8_t> encoded) {
    if (!IsPacketValid(item)) {
      return;
    }
    if (encoded.size() &&
        !IsGuestRangeValid(item.payload_ptr, encoded.size())) {
      CompleteError(item, false);
      return;
    }
    if (!encoded.empty()) {
      auto* payload =
          REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(item.payload_ptr);
      std::copy(encoded.begin(), encoded.end(), payload);
    }
    auto* packet = REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(item.packet_ptr);
    const uint32_t encoded_size = static_cast<uint32_t>(encoded.size());
    memory::store_and_swap<uint32_t>(packet + 12, encoded_size);
    memory::store_and_swap<uint32_t>(packet + 4, encoded_size);
    std::atomic_thread_fence(std::memory_order_release);
    memory::store_and_swap<uint32_t>(packet + 0, 0);
  }

  static void CompletePlayback(const VoiceWorkItem& item) {
    if (!IsPacketValid(item)) return;
    auto* packet = REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(item.packet_ptr);
    memory::store_and_swap<uint32_t>(packet + 4, item.payload_size);
    std::atomic_thread_fence(std::memory_order_release);
    memory::store_and_swap<uint32_t>(packet + 0, 0);
  }

  bool WaitForWork(rex::compat::stop_token stop_token, bool capture,
                   VoiceWorkItem& item) {
    std::unique_lock lock(mutex);
    condition.wait(lock, [&] {
      return stopping || stop_token.stop_requested() ||
             !(capture ? capture_queue : playback_queue).empty();
    });
    if (stopping || stop_token.stop_requested()) return false;
    auto& queue = capture ? capture_queue : playback_queue;
    item = std::move(queue.front());
    queue.pop_front();
    return true;
  }

  bool IsStopping() const {
    std::lock_guard lock(mutex);
    return stopping;
  }

  void CaptureWorkerMain(rex::compat::stop_token stop_token) {
    VoiceWorkItem item;
    while (WaitForWork(stop_token, true, item)) {
      if (!audio_device || !capture_codec || !item.payload_size) {
        CompleteError(item, false);
        continue;
      }
      std::vector<int16_t> samples(
          static_cast<size_t>(item.payload_size) * kSamplesPerEncodedByte);
      const size_t captured = audio_device->Capture(samples, kCaptureTimeout);
      if (IsStopping()) {
        CompleteError(item, false);
        continue;
      }

      std::vector<uint8_t> encoded;
      encoded.reserve(item.payload_size);
      size_t sample_index = 0;
      bool codec_ok = true;
      while (sample_index + 1 < captured) {
        uint8_t low = 0;
        uint8_t high = 0;
        if (!capture_codec->Encode(samples[sample_index], low) ||
            !capture_codec->Encode(samples[sample_index + 1], high)) {
          codec_ok = false;
          break;
        }
        encoded.push_back(static_cast<uint8_t>(
            (low & kVoiceNibbleMask) |
            ((high & kVoiceNibbleMask) << kVoiceNibbleShift)));
        sample_index += kSamplesPerEncodedByte;
      }
      if (!codec_ok || encoded.empty()) {
        CompleteError(item, false);
      } else {
        CompleteCapture(item, encoded);
      }
    }
  }

  void PlaybackWorkerMain(rex::compat::stop_token stop_token) {
    VoiceWorkItem item;
    while (WaitForWork(stop_token, false, item)) {
      if (!audio_device || !playback_codec || item.playback_payload.empty()) {
        CompleteError(item, true);
        continue;
      }
      std::vector<int16_t> samples;
      samples.reserve(item.playback_payload.size() * kSamplesPerEncodedByte);
      bool codec_ok = true;
      for (const uint8_t byte : item.playback_payload) {
        int16_t low_sample = 0;
        int16_t high_sample = 0;
        if (!playback_codec->Decode(byte & kVoiceNibbleMask, low_sample) ||
            !playback_codec->Decode(
                static_cast<uint8_t>(byte >> kVoiceNibbleShift), high_sample)) {
          codec_ok = false;
          break;
        }
        samples.push_back(low_sample);
        samples.push_back(high_sample);
      }
      if (IsStopping() || !codec_ok || !audio_device->Play(samples)) {
        CompleteError(item, true);
      } else {
        CompletePlayback(item);
      }
    }
  }

  mutable std::mutex mutex;
  std::condition_variable condition;
  std::deque<VoiceWorkItem> capture_queue;
  std::deque<VoiceWorkItem> playback_queue;
  bool stopping = false;
  std::once_flag stop_once;
  rex::compat::jthread capture_worker;
  rex::compat::jthread playback_worker;
};

std::mutex g_voice_mutex;
std::unordered_map<uint32_t, std::shared_ptr<VoiceHandleState>> g_voice_handles;
std::atomic<uint32_t> g_next_voice_handle{1};

bool IsGuestRangeValid(uint32_t address, size_t size) {
  if (!address || !size || size > std::numeric_limits<uint32_t>::max()) return false;
  const uint64_t end = static_cast<uint64_t>(address) + size - 1;
  return end <= std::numeric_limits<uint32_t>::max() &&
         REX_KERNEL_MEMORY()->LookupHeap(address) &&
         REX_KERNEL_MEMORY()->LookupHeap(static_cast<uint32_t>(end));
}

}  // namespace

u32 XamVoiceIsActiveProcess_entry() {
  auto* live = REX_KERNEL_STATE()->live_compatibility();
  return live && live->voice_audio_device() && live->voice_sample_codec() ? 1 : 0;
}

u32 XamVoiceCreate_entry(u32 unk1,  // 0
                         u32 unk2,  // 0xF
                         mapped_u32 out_voice_ptr) {
  if (!out_voice_ptr || unk2 != 0xF) return X_E_INVALIDARG;
  auto* live = REX_KERNEL_STATE()->live_compatibility();
  if (!live || !live->config().voice_audio_device ||
      !live->config().voice_sample_codec) {
    out_voice_ptr.Zero();
    return X_STATUS_NOT_SUPPORTED;
  }
  const auto audio_device = live->config().voice_audio_device;
  const auto codec = live->config().voice_sample_codec;
  if (!audio_device->Open()) {
    REXSYS_WARN("XamVoiceCreate could not open a capture or playback device: {}",
                audio_device->last_error());
    out_voice_ptr.Zero();
    return X_E_DEVICE_NOT_CONNECTED;
  }
  auto capture_codec = codec->CreateState();
  auto playback_codec = codec->CreateState();
  if (!capture_codec || !playback_codec) {
    audio_device->Close();
    out_voice_ptr.Zero();
    return X_E_FAIL;
  }
  auto state = std::make_shared<VoiceHandleState>(
      audio_device, std::move(capture_codec), std::move(playback_codec));
  state->Start();
  uint32_t handle = g_next_voice_handle.fetch_add(1, std::memory_order_relaxed);
  if (!handle) handle = g_next_voice_handle.fetch_add(1, std::memory_order_relaxed);
  {
    std::lock_guard lock(g_voice_mutex);
    g_voice_handles.emplace(handle, state);
  }
  *out_voice_ptr = handle;
  REXSYS_INFO("XamVoiceCreate opened handle {}: capture={}, playback={}", handle,
              audio_device->capture_available(), audio_device->playback_available());
  return X_ERROR_SUCCESS;
}

u32 XamVoiceClose_entry(u32 voice_handle) {
  std::shared_ptr<VoiceHandleState> state;
  {
    std::lock_guard lock(g_voice_mutex);
    auto found = g_voice_handles.find(voice_handle);
    if (found == g_voice_handles.end()) {
      return X_HRESULT_FROM_WIN32(X_ERROR_INVALID_HANDLE);
    }
    state = std::move(found->second);
    g_voice_handles.erase(found);
  }
  state->Stop();
  return X_ERROR_SUCCESS;
}

u32 XamVoiceHeadsetPresent_entry(u32 voice_handle) {
  std::lock_guard lock(g_voice_mutex);
  auto found = g_voice_handles.find(voice_handle);
  return found != g_voice_handles.end() && found->second->capture_available() ? 1 : 0;
}

u32 XamVoiceSubmitPacket_entry(u32 voice_handle, u32 mode, u32 packet_ptr) {
  if (mode > 1 || !IsGuestRangeValid(packet_ptr, kVoicePacketHeaderSize)) {
    return X_E_INVALIDARG;
  }
  auto* packet = REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(packet_ptr);
  const uint32_t payload_ptr = memory::load_and_swap<uint32_t>(packet + 8);
  const uint32_t payload_size = memory::load_and_swap<uint32_t>(packet + 12);
  const uint32_t codec_tag = memory::load_and_swap<uint32_t>(packet + 20);
  if (payload_size > kMaximumVoicePacketBytes ||
      (payload_size && !IsGuestRangeValid(payload_ptr, payload_size)) ||
      codec_tag != kVoiceCodecTag) {
    return X_E_INVALIDARG;
  }

  std::shared_ptr<VoiceHandleState> state;
  {
    std::lock_guard lock(g_voice_mutex);
    auto found = g_voice_handles.find(voice_handle);
    if (found == g_voice_handles.end()) {
      return X_HRESULT_FROM_WIN32(X_ERROR_INVALID_HANDLE);
    }
    state = found->second;
  }

  VoiceWorkItem item{packet_ptr, payload_ptr, payload_size, {}};
  if (mode == 0 && payload_size) {
    const auto* payload =
        REX_KERNEL_MEMORY()->TranslateVirtual<const uint8_t*>(payload_ptr);
    item.playback_payload.assign(payload, payload + payload_size);
  }

  memory::store_and_swap<uint32_t>(packet + 4, 0);
  memory::store_and_swap<uint32_t>(packet + 0, kVoicePacketPending);
  const bool queued = mode == 1 ? state->EnqueueCapture(std::move(item))
                                : state->EnqueuePlayback(std::move(item));
  if (!queued) {
    memory::store_and_swap<uint32_t>(packet + 4, mode == 0 ? payload_size : 0);
    std::atomic_thread_fence(std::memory_order_release);
    memory::store_and_swap<uint32_t>(
        packet + 0, static_cast<uint32_t>(X_STATUS_UNSUCCESSFUL));
    return X_HRESULT_FROM_WIN32(X_ERROR_INVALID_HANDLE);
  }
  return X_ERROR_SUCCESS;
}

}  // namespace xam
}  // namespace kernel
}  // namespace rex

REX_EXPORT(__imp__XamVoiceIsActiveProcess, rex::kernel::xam::XamVoiceIsActiveProcess_entry)
REX_EXPORT(__imp__XamVoiceCreate, rex::kernel::xam::XamVoiceCreate_entry)
REX_EXPORT(__imp__XamVoiceClose, rex::kernel::xam::XamVoiceClose_entry)
REX_EXPORT(__imp__XamVoiceHeadsetPresent, rex::kernel::xam::XamVoiceHeadsetPresent_entry)
REX_EXPORT(__imp__XamVoiceSubmitPacket, rex::kernel::xam::XamVoiceSubmitPacket_entry)

REX_EXPORT_STUB(__imp__XamMuteSound);
REX_EXPORT_STUB(__imp__XamVoiceDisableMicArray);
REX_EXPORT_STUB(__imp__XamVoiceGetBatteryStatus);
REX_EXPORT_STUB(__imp__XamVoiceGetDirectionalData);
REX_EXPORT_STUB(__imp__XamVoiceGetMicArrayAudio);
REX_EXPORT_STUB(__imp__XamVoiceGetMicArrayAudioEx);
REX_EXPORT_STUB(__imp__XamVoiceGetMicArrayFilenameDesc);
REX_EXPORT_STUB(__imp__XamVoiceGetMicArrayStatus);
REX_EXPORT_STUB(__imp__XamVoiceGetMicArrayUnderrunStatus);
REX_EXPORT_STUB(__imp__XamVoiceMuteMicArray);
REX_EXPORT_STUB(__imp__XamVoiceRecordUserPrivileges);
REX_EXPORT_STUB(__imp__XamVoiceSetAudioCaptureRoutine);
REX_EXPORT_STUB(__imp__XamVoiceSetMicArrayBeamAngle);
REX_EXPORT_STUB(__imp__XamVoiceSetMicArrayIdleUsers);
