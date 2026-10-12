#pragma once
// Removes static ClipDistance/CullDistance usage from precompiled SPIR-V on
// devices without the matching Vulkan features (Mali exposes neither).
//
// The native stock shader cache was compiled offline assuming desktop-class
// features, so vertex shaders can declare `BuiltIn ClipDistance` even for
// draws that never enable a user clip plane. Using the built-in at all
// requires shaderClipDistance/shaderCullDistance on the logical device;
// the ARM Mali driver answers a pipeline built without them with a segfault
// (addr=0x50 in cmpbe_v2_compile_multiple_shaders) instead of an error.
//
// The rewrite replaces the BuiltIn decoration with a plain, unconsumed
// Location output (same word count, no ID churn) and drops the now-unused
// capability. Draws that really use clip planes stay rejected by the
// draw-time shader-clip-distance-unsupported check; draws with no planes
// render exactly as before, since the writes were no-ops there.
#include <cstdint>
#include <optional>
#include <vector>

namespace rex::graphics::gta4_native {

struct SpirvClipStripResult {
  std::vector<uint32_t> words;
  uint32_t stripped_decorations = 0;
};

inline std::optional<SpirvClipStripResult> StripSpirvClipCullDistance(
    const std::vector<uint32_t>& in, bool strip_clip, bool strip_cull) {
  constexpr uint16_t kOpCapability = 17, kOpDecorate = 71, kOpMemberDecorate = 72;
  constexpr uint32_t kDecorationLocation = 30, kDecorationBuiltIn = 11;
  constexpr uint32_t kBuiltInClipDistance = 3, kBuiltInCullDistance = 4;
  constexpr uint32_t kCapabilityClipDistance = 32, kCapabilityCullDistance = 33;
  if (!strip_clip && !strip_cull) return std::nullopt;
  if (in.size() < 5 || in[0] != 0x07230203u) return std::nullopt;

  // Highest Location already used, so the replacement picks a free one.
  // Input and output location spaces are separate, but sharing the same
  // counter is harmless and keeps the scan to one pass.
  uint32_t max_location = 0;
  bool found = false;
  for (size_t i = 5; i < in.size();) {
    const uint32_t count = in[i] >> 16;
    const uint16_t op = uint16_t(in[i] & 0xFFFF);
    if (!count || i + count > in.size()) return std::nullopt;
    const uint32_t* w = in.data() + i;
    if ((op == kOpDecorate && count >= 4 && w[2] == kDecorationLocation) ||
        (op == kOpMemberDecorate && count >= 5 && w[3] == kDecorationLocation)) {
      const uint32_t location = op == kOpDecorate ? w[3] : w[4];
      if (!found || location > max_location) {
        max_location = location;
        found = true;
      }
    }
    if (((op == kOpDecorate && count >= 4 && w[2] == kDecorationBuiltIn) ||
         (op == kOpMemberDecorate && count >= 5 && w[3] == kDecorationBuiltIn))) {
      const uint32_t builtin = op == kOpDecorate ? w[3] : w[4];
      if ((builtin == kBuiltInClipDistance && strip_clip) ||
          (builtin == kBuiltInCullDistance && strip_cull)) {
        found = true;
      }
    }
    i += count;
  }

  uint32_t next_location = found ? max_location + 1 : 0;
  SpirvClipStripResult result;
  std::vector<uint32_t>& out = result.words;
  out.reserve(in.size());
  out.insert(out.end(), in.begin(), in.begin() + 5);
  for (size_t i = 5; i < in.size();) {
    const uint32_t count = in[i] >> 16;
    const uint16_t op = uint16_t(in[i] & 0xFFFF);
    const uint32_t* w = in.data() + i;
    if (op == kOpCapability && count == 2 &&
        ((w[1] == kCapabilityClipDistance && strip_clip) ||
         (w[1] == kCapabilityCullDistance && strip_cull))) {
      // Drop the capability: nothing references it after the rewrite below.
      i += count;
      continue;
    }
    const bool is_member = op == kOpMemberDecorate;
    if ((op == kOpDecorate || is_member) &&
        w[is_member ? 3 : 2] == kDecorationBuiltIn) {
      const size_t value_index = is_member ? 4 : 3;
      const uint32_t builtin = w[value_index];
      if ((builtin == kBuiltInClipDistance && strip_clip) ||
          (builtin == kBuiltInCullDistance && strip_cull)) {
        if (next_location >= 32) return std::nullopt;
        out.insert(out.end(), w, w + count);
        // Same word layout: decoration id, then the new value.
        out[out.size() - count + (is_member ? 3 : 2)] = kDecorationLocation;
        out[out.size() - count + value_index] = next_location++;
        ++result.stripped_decorations;
        i += count;
        continue;
      }
    }
    out.insert(out.end(), w, w + count);
    i += count;
  }
  if (!result.stripped_decorations) return std::nullopt;
  return result;
}

}  // namespace rex::graphics::gta4_native
