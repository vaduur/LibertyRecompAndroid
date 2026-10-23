/**
 * @file native_spirv_location_remap_test.cpp
 * @brief GTA IV native-renderer vertex input location compaction tests.
 */

#include <cstdint>
#include <initializer_list>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "graphics/gta4_native/native_spirv_location_remap.h"

namespace rex::graphics::gta4_native {
namespace {

void AppendInstruction(std::vector<uint32_t>& module, uint32_t opcode,
                       std::initializer_list<uint32_t> operands) {
  module.push_back(((uint32_t(operands.size()) + 1u) << 16) | opcode);
  module.insert(module.end(), operands.begin(), operands.end());
}

std::vector<uint32_t> MakeVertexModule(const std::vector<uint32_t>& locations) {
  std::vector<uint32_t> module = {kNativeSpirvMagic, 0x10000, 0,
                                  uint32_t(locations.size() * 2 + 10), 0};
  AppendInstruction(module, kNativeSpirvOpEntryPoint,
                    {kNativeSpirvExecutionModelVertex, 1, 0});
  uint32_t id = 10;
  for (const uint32_t location : locations) {
    AppendInstruction(module, kNativeSpirvOpDecorate,
                      {id++, kNativeSpirvDecorationLocation, location});
  }
  id = 10;
  for (size_t i = 0; i < locations.size(); ++i) {
    AppendInstruction(module, kNativeSpirvOpVariable,
                      {2, id++, kNativeSpirvStorageClassInput});
  }
  return module;
}

std::vector<uint32_t> ReadLocations(const std::vector<uint32_t>& module) {
  std::vector<uint32_t> locations;
  size_t cursor = 5;
  while (cursor < module.size()) {
    const uint32_t word_count = module[cursor] >> 16;
    const uint32_t opcode = module[cursor] & 0xFFFF;
    if (opcode == kNativeSpirvOpDecorate &&
        module[cursor + 2] == kNativeSpirvDecorationLocation) {
      locations.push_back(module[cursor + 3]);
    }
    cursor += word_count;
  }
  return locations;
}

// The legacy table's locations, including the two the fixed table could not
// place under sixteen: TEXCOORD6 (22) and COLOR1 (32).
constexpr uint32_t kLegacyLocations[] = {0, 4, 13, 14, 15, 16, 17, 20, 22, 32};

// How many decorations the compaction changes: a location whose slot already
// is its index (the first one) is left alone.
uint32_t CountChanged(const std::vector<uint32_t>& legacy) {
  uint32_t changed = 0;
  for (size_t i = 0; i < legacy.size(); ++i) {
    if (uint32_t(i) != legacy[i]) {
      ++changed;
    }
  }
  return changed;
}

}  // namespace

TEST_CASE("Vertex input locations are compacted to what the device can address", "[unit]") {
  const std::vector<uint32_t> legacy(std::begin(kLegacyLocations),
                                     std::end(kLegacyLocations));
  std::vector<uint32_t> early = MakeVertexModule(legacy);
  std::vector<uint32_t> late;
  std::vector<uint32_t> map;

  const uint32_t changed = RemapSpirvVertexInputLocations(early, late, map);
  REQUIRE(changed == CountChanged(legacy));
  REQUIRE(map.size() == legacy.size());

  const std::vector<uint32_t> compact = ReadLocations(early);
  REQUIRE(compact.size() == legacy.size());
  for (size_t i = 0; i < compact.size(); ++i) {
    // The slots are packed down to 0..n-1 in ascending legacy order.
    REQUIRE(compact[i] == uint32_t(i));
    // The map carries the legacy location back for the vertex declaration.
    REQUIRE(map[compact[i]] == legacy[i]);
  }
  for (const uint32_t location : compact) {
    REQUIRE(location < 16);
  }
}

TEST_CASE("A fragment module is not compacted", "[unit]") {
  std::vector<uint32_t> fragment = MakeVertexModule({0, 1});
  fragment[6] = 4;  // ExecutionModel Fragment
  std::vector<uint32_t> none;
  std::vector<uint32_t> map;

  REQUIRE(RemapSpirvVertexInputLocations(fragment, none, map) == 0);
  REQUIRE(map.empty());
  REQUIRE(ReadLocations(fragment) == std::vector<uint32_t>{0, 1});
}

TEST_CASE("The compaction is skipped when the device can address the locations", "[unit]") {
  const std::vector<uint32_t> legacy(std::begin(kLegacyLocations),
                                     std::end(kLegacyLocations));
  std::vector<uint32_t> early = MakeVertexModule(legacy);
  std::vector<uint32_t> late;
  std::vector<uint32_t> map;

  // A limit of 32 covers the highest legacy location (32 is not below it).
  const uint32_t changed =
      RemapSpirvVertexInputLocationsIfNeeded(early, late, map, 33);
  REQUIRE(changed == 0);
  REQUIRE(map.empty());
  REQUIRE(ReadLocations(early) == legacy);
}

TEST_CASE("The compaction runs when the device cannot address the locations", "[unit]") {
  const std::vector<uint32_t> legacy(std::begin(kLegacyLocations),
                                     std::end(kLegacyLocations));
  std::vector<uint32_t> early = MakeVertexModule(legacy);
  std::vector<uint32_t> late;
  std::vector<uint32_t> map;

  // The Vulkan minimum of 16 leaves COLOR0 at 17 and every pipeline over it.
  const uint32_t changed = RemapSpirvVertexInputLocationsIfNeeded(early, late, map, 16);
  REQUIRE(changed == CountChanged(legacy));
  REQUIRE(map.size() == legacy.size());

  const std::vector<uint32_t> compact = ReadLocations(early);
  for (size_t i = 0; i < compact.size(); ++i) {
    REQUIRE(compact[i] == uint32_t(i));
    REQUIRE(map[compact[i]] == legacy[i]);
  }
}

TEST_CASE("The compaction can be forced on a device that does not need it", "[unit]") {
  const std::vector<uint32_t> legacy(std::begin(kLegacyLocations),
                                     std::end(kLegacyLocations));
  std::vector<uint32_t> early = MakeVertexModule(legacy);
  std::vector<uint32_t> late;
  std::vector<uint32_t> map;

  // A limit of zero is how the diagnostics cvar forces the remap.
  const uint32_t changed = RemapSpirvVertexInputLocationsIfNeeded(early, late, map, 0);
  REQUIRE(changed == CountChanged(legacy));
  REQUIRE(map.size() == legacy.size());
  REQUIRE(ReadLocations(early).size() == legacy.size());
}

}  // namespace rex::graphics::gta4_native
