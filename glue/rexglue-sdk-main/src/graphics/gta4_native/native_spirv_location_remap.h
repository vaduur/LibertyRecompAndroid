#pragma once
// Compacts the recompiled vertex shaders' input locations.
//
// ConvertVertexUsageToLocation() assigns each Xbox 360 vertex semantic a slot
// from a fixed table whose highest entry is 39, so the renderer implicitly
// needs 40 vertex input locations. Adreno exposes 32 and gets away with it
// because GTA IV stays inside the 0..31 range; PowerVR (Tensor G5/G6) reports
// maxVertexInputAttributes = 16, the Vulkan minimum, and the sparse table
// pushes COLOR0 to 17, BLENDINDICES0 to 18, BLENDWEIGHT0 to 19 and
// TEXCOORD3..5 to 16/20/21. COLOR0 appears in every declaration the game
// registers, so every pipeline is rejected and nothing renders at all.
//
// A single global table cannot fix this: forty semantics do not fit in sixteen
// slots. No individual shader needs them all, though - a vertex shader with
// more than sixteen inputs could not run on such a device in any case - so the
// map is built per shader from the Location decorations the module actually
// carries, which are packed down to 0..n-1 in ascending order. The map is kept
// alongside the reflected inputs so the declaration can still be matched by the
// semantic's original location. This mirrors native_spirv_descriptor_remap.h,
// which repacks descriptor sets for the same kind of device limit.
#include <algorithm>
#include <cstdint>
#include <vector>

#include "native_spirv_reflection.h"

namespace rex::graphics::gta4_native {

constexpr uint32_t kNativeSpirvOpEntryPoint = 15u;
constexpr uint32_t kNativeSpirvOpVariable = 59u;
constexpr uint32_t kNativeSpirvOpDecorate = 71u;
constexpr uint32_t kNativeSpirvExecutionModelVertex = 0u;
constexpr uint32_t kNativeSpirvStorageClassInput = 1u;
constexpr uint32_t kNativeSpirvMagic = 0x07230203u;

// Ids of the Input variables of a Vertex entry point. A module that is not a
// vertex shader yields none: a vertex shader's outputs and a fragment shader's
// inputs form the interface between the two stages and must keep their
// original numbering.
inline bool CollectSpirvVertexInputVariables(const std::vector<uint32_t>& spirv,
                                             std::vector<bool>& input_variable) {
  if (spirv.size() < 5 || spirv[0] != kNativeSpirvMagic || !spirv[3]) {
    return false;
  }
  const uint32_t id_bound = spirv[3];
  input_variable.assign(id_bound, false);
  bool vertex_stage = false;
  // Global OpVariable instructions follow the decoration block, so the
  // decorations cannot be rewritten before this pass has run.
  size_t cursor = 5;
  while (cursor < spirv.size()) {
    const uint32_t instruction = spirv[cursor];
    const uint32_t word_count = instruction >> 16;
    const uint32_t opcode = instruction & 0xFFFFu;
    if (!word_count || word_count > spirv.size() - cursor) {
      return false;
    }
    if (opcode == kNativeSpirvOpEntryPoint && word_count >= 2) {
      vertex_stage = spirv[cursor + 1] == kNativeSpirvExecutionModelVertex;
    } else if (opcode == kNativeSpirvOpVariable && word_count >= 4) {
      const uint32_t result_id = spirv[cursor + 2];
      if (result_id < id_bound && spirv[cursor + 3] == kNativeSpirvStorageClassInput) {
        input_variable[result_id] = true;
      }
    }
    cursor += word_count;
  }
  return vertex_stage;
}

// Adds this module's vertex input locations to `legacy_locations`, which stays
// sorted and free of duplicates so that the compact slot of a location is its
// index. Both modules of a shader contribute: they are two compilations of the
// same guest shader and the pipeline describes its attributes once.
inline void CollectSpirvVertexInputLocations(const std::vector<uint32_t>& spirv,
                                             std::vector<uint32_t>& legacy_locations) {
  std::vector<bool> input_variable;
  if (!CollectSpirvVertexInputVariables(spirv, input_variable)) {
    return;
  }
  const uint32_t id_bound = spirv[3];
  size_t cursor = 5;
  while (cursor < spirv.size()) {
    const uint32_t instruction = spirv[cursor];
    const uint32_t word_count = instruction >> 16;
    const uint32_t opcode = instruction & 0xFFFFu;
    if (!word_count || word_count > spirv.size() - cursor) {
      return;
    }
    if (opcode == kNativeSpirvOpDecorate && word_count >= 4 &&
        spirv[cursor + 2] == kNativeSpirvDecorationLocation) {
      const uint32_t target = spirv[cursor + 1];
      if (target < id_bound && input_variable[target]) {
        const uint32_t location = spirv[cursor + 3];
        const auto at = std::lower_bound(legacy_locations.begin(), legacy_locations.end(), location);
        if (at == legacy_locations.end() || *at != location) {
          legacy_locations.insert(at, location);
        }
      }
    }
    cursor += word_count;
  }
}

// Rewrites the Location decorations of a vertex module's input variables to
// their index in `legacy_by_compact`. Returns the number of decorations
// changed. A location the map does not contain is left alone: that only
// happens for a shader override whose inputs differ from the stock shader's,
// and the interface comparison that follows has to see the difference.
inline uint32_t ApplySpirvVertexInputLocations(std::vector<uint32_t>& spirv,
                                               const std::vector<uint32_t>& legacy_by_compact) {
  std::vector<bool> input_variable;
  if (legacy_by_compact.empty() || !CollectSpirvVertexInputVariables(spirv, input_variable)) {
    return 0;
  }
  const uint32_t id_bound = spirv[3];
  uint32_t remapped = 0;
  size_t cursor = 5;
  while (cursor < spirv.size()) {
    const uint32_t instruction = spirv[cursor];
    const uint32_t word_count = instruction >> 16;
    const uint32_t opcode = instruction & 0xFFFFu;
    if (!word_count || word_count > spirv.size() - cursor) {
      break;
    }
    if (opcode == kNativeSpirvOpDecorate && word_count >= 4 &&
        spirv[cursor + 2] == kNativeSpirvDecorationLocation) {
      const uint32_t target = spirv[cursor + 1];
      if (target < id_bound && input_variable[target]) {
        const uint32_t legacy = spirv[cursor + 3];
        const auto at = std::lower_bound(legacy_by_compact.begin(), legacy_by_compact.end(), legacy);
        if (at != legacy_by_compact.end() && *at == legacy) {
          const uint32_t compact = uint32_t(at - legacy_by_compact.begin());
          if (compact != legacy) {
            spirv[cursor + 3] = compact;
            ++remapped;
          }
        }
      }
    }
    cursor += word_count;
  }
  return remapped;
}

// Builds the map for a shader's two modules and applies it to both.
inline uint32_t RemapSpirvVertexInputLocations(std::vector<uint32_t>& early_spirv,
                                               std::vector<uint32_t>& late_spirv,
                                               std::vector<uint32_t>& legacy_by_compact) {
  legacy_by_compact.clear();
  CollectSpirvVertexInputLocations(early_spirv, legacy_by_compact);
  CollectSpirvVertexInputLocations(late_spirv, legacy_by_compact);
  return ApplySpirvVertexInputLocations(early_spirv, legacy_by_compact) +
         ApplySpirvVertexInputLocations(late_spirv, legacy_by_compact);
}

// Compacts the vertex input locations only when the device cannot address
// them where the guest's semantics put them.
//
// A device whose limit covers the highest legacy location keeps them: its
// shader interface stays the one the guest declared and the pipeline cache
// keeps its entries. A device below that limit (the Vulkan minimum of 16,
// which is what PowerVR and Mali report) cannot address location 17 and up,
// so the locations are packed down to 0..n-1 and the originals are kept in
// the map for the vertex declaration to be matched against.
//
// A `max_vertex_input_attributes` of zero forces the compaction, which is how
// the remap is exercised on a device that does not need it.
inline uint32_t RemapSpirvVertexInputLocationsIfNeeded(std::vector<uint32_t>& early_spirv,
                                                        std::vector<uint32_t>& late_spirv,
                                                        std::vector<uint32_t>& legacy_by_compact,
                                                        uint32_t max_vertex_input_attributes) {
  legacy_by_compact.clear();
  CollectSpirvVertexInputLocations(early_spirv, legacy_by_compact);
  CollectSpirvVertexInputLocations(late_spirv, legacy_by_compact);
  if (legacy_by_compact.empty() ||
      legacy_by_compact.back() < max_vertex_input_attributes) {
    // Nothing to compact: the map stays empty and the reflection falls back
    // to the location the module already carries.
    legacy_by_compact.clear();
    return 0;
  }
  return ApplySpirvVertexInputLocations(early_spirv, legacy_by_compact) +
         ApplySpirvVertexInputLocations(late_spirv, legacy_by_compact);
}

}  // namespace rex::graphics::gta4_native
