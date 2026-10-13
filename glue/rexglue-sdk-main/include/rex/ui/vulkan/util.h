#pragma once
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

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include <rex/math.h>
#include <rex/ui/vulkan/device.h>

namespace rex {
namespace ui {
namespace vulkan {
namespace util {

template <typename DestroyFunction, typename Object>
inline bool DestroyAndNullHandle(DestroyFunction* const destroy_function, Object& handle) {
  if (handle != VK_NULL_HANDLE) {
    destroy_function(handle, nullptr);
    handle = VK_NULL_HANDLE;
    return true;
  }
  return false;
}

template <typename DestroyFunction, typename Parent, typename Object>
inline bool DestroyAndNullHandle(DestroyFunction* const destroy_function, const Parent parent,
                                 Object& handle) {
  if (handle != VK_NULL_HANDLE) {
    destroy_function(parent, handle, nullptr);
    handle = VK_NULL_HANDLE;
    return true;
  }
  return false;
}

enum class MemoryPurpose {
  kDeviceLocal,
  kUpload,
  kReadback,
};

// Exact support for ordinary (pNext-null) image creation tuples and extents.
bool IsImageCreateInfoSupported(const VulkanDevice* vulkan_device,
                                const VkImageCreateInfo& create_info);

inline uint32_t ChooseHostMemoryType(const VulkanDevice::MemoryTypes& memory_types,
                                     uint32_t supported_types, const bool is_readback) {
  supported_types &= memory_types.host_visible;
  uint32_t memory_type;
  // For upload, uncached is preferred so writes do not pollute the CPU cache.
  // For readback, cached is preferred so multiple CPU reads are fast.
  // If the preferred caching behavior is not available, pick any host-visible.
  if (rex::bit_scan_forward(
          supported_types & (is_readback ? memory_types.host_cached : ~memory_types.host_cached),
          &memory_type) ||
      rex::bit_scan_forward(supported_types, &memory_type)) {
    return memory_type;
  }
  return UINT32_MAX;
}

inline uint32_t ChooseMemoryType(const VulkanDevice::MemoryTypes& memory_types,
                                 const uint32_t supported_types, const MemoryPurpose purpose) {
  switch (purpose) {
    case MemoryPurpose::kDeviceLocal: {
      uint32_t memory_type;
      if (rex::bit_scan_forward(supported_types & memory_types.device_local, &memory_type)) {
        return memory_type;
      }
      if (rex::bit_scan_forward(supported_types, &memory_type)) {
        return memory_type;
      }
      return UINT32_MAX;
    } break;
    case MemoryPurpose::kUpload:
    case MemoryPurpose::kReadback:
      return ChooseHostMemoryType(memory_types, supported_types,
                                  purpose == MemoryPurpose::kReadback);
    default:
      assert_unhandled_case(purpose);
      return UINT32_MAX;
  }
}

// Actual memory size is required if explicit size is specified for clamping to
// the actual memory allocation size while rounding to the non-coherent atom
// size (offset + size passed to vkFlushMappedMemoryRanges inside this function
// must be either a multiple of nonCoherentAtomSize (but not exceeding the
// memory size) or equal to the memory size).
void FlushMappedMemoryRange(const VulkanDevice* vulkan_device, VkDeviceMemory memory,
                            uint32_t memory_type, VkDeviceSize offset = 0,
                            VkDeviceSize memory_size = VK_WHOLE_SIZE,
                            VkDeviceSize size = VK_WHOLE_SIZE);

inline VkExtent2D GetMax2DFramebufferExtent(const VulkanDevice::Properties& device_properties) {
  VkExtent2D max_extent;
  max_extent.width =
      std::min(device_properties.maxFramebufferWidth, device_properties.maxImageDimension2D);
  max_extent.height =
      std::min(device_properties.maxFramebufferHeight, device_properties.maxImageDimension2D);
  return max_extent;
}

inline VkImageSubresourceRange InitializeSubresourceRange(
    const VkImageAspectFlags aspect_mask = VK_IMAGE_ASPECT_COLOR_BIT,
    const uint32_t base_mip_level = 0, const uint32_t level_count = VK_REMAINING_MIP_LEVELS,
    const uint32_t base_array_layer = 0, const uint32_t layer_count = VK_REMAINING_ARRAY_LAYERS) {
  VkImageSubresourceRange range;
  range.aspectMask = aspect_mask;
  range.baseMipLevel = base_mip_level;
  range.levelCount = level_count;
  range.baseArrayLayer = base_array_layer;
  range.layerCount = layer_count;
  return range;
}

// Creates a buffer backed by a dedicated allocation. The allocation size will
// NOT be aligned to nonCoherentAtomSize - if mapping or flushing not the whole
// size, memory_size_out must be used for clamping the range.
bool CreateDedicatedAllocationBuffer(const VulkanDevice* vulkan_device, VkDeviceSize size,
                                     VkBufferUsageFlags usage, MemoryPurpose memory_purpose,
                                     VkBuffer& buffer_out, VkDeviceMemory& memory_out,
                                     uint32_t* memory_type_out = nullptr,
                                     VkDeviceSize* memory_size_out = nullptr);

bool CreateDedicatedAllocationImage(const VulkanDevice* vulkan_device,
                                    const VkImageCreateInfo& create_info,
                                    MemoryPurpose memory_purpose, VkImage& image_out,
                                    VkDeviceMemory& memory_out, uint32_t* memory_type_out = nullptr,
                                    VkDeviceSize* memory_size_out = nullptr);

// Mali GPUs (and some others) report shaderClipDistance = false. Shaders that
// write gl_ClipDistance are then invalid there. This rewrites a SPIR-V module so
// each standalone ClipDistance output becomes an ordinary, unused output at a
// free Location and drops the ClipDistance capability. Clipping against user
// planes is lost, but the shader becomes legal. Returns false if the module
// uses a form this cannot rewrite (e.g. ClipDistance inside a gl_PerVertex
// block); the module is then left unchanged.
inline bool SpirvUsesClipDistance(const uint32_t* code, size_t word_count) {
  for (size_t i = 5; i < word_count;) {
    const uint32_t count = code[i] >> 16;
    if (!count || i + count > word_count) return false;
    if ((code[i] & 0xFFFF) == 17 && count >= 2 && code[i + 1] == 32) return true;
    i += count;
  }
  return false;
}

inline bool StripSpirvClipDistance(std::vector<uint32_t>& words) {
  constexpr uint32_t kOpCapability = 17, kOpTypeArray = 28, kOpTypePointer = 32,
                     kOpConstant = 43, kOpVariable = 59, kOpDecorate = 71,
                     kOpMemberDecorate = 72;
  constexpr uint32_t kDecorationBuiltIn = 11, kDecorationLocation = 30;
  constexpr uint32_t kBuiltInClipDistance = 3, kCapabilityClipDistance = 32;
  if (words.size() < 5 || words[0] != 0x07230203u) return false;

  std::unordered_map<uint32_t, uint32_t> pointer_pointee, variable_type, constant_value;
  std::unordered_map<uint32_t, uint32_t> array_length_id;
  uint32_t next_location = 0;
  bool found = false;
  for (size_t i = 5; i < words.size();) {
    const uint32_t count = words[i] >> 16, op = words[i] & 0xFFFF;
    if (!count || i + count > words.size()) return false;
    if (op == kOpTypePointer && count >= 4) pointer_pointee[words[i + 1]] = words[i + 3];
    if (op == kOpTypeArray && count >= 4) array_length_id[words[i + 1]] = words[i + 3];
    if (op == kOpConstant && count >= 4) constant_value[words[i + 2]] = words[i + 3];
    if (op == kOpVariable && count >= 4) variable_type[words[i + 2]] = words[i + 1];
    if (op == kOpDecorate && count >= 4 && words[i + 2] == kDecorationLocation)
      next_location = std::max(next_location, words[i + 3] + 1);
    if (op == kOpDecorate && count >= 4 && words[i + 2] == kDecorationBuiltIn &&
        words[i + 3] == kBuiltInClipDistance)
      found = true;
    if (op == kOpMemberDecorate && count >= 5 && words[i + 3] == kDecorationBuiltIn &&
        words[i + 4] == kBuiltInClipDistance)
      return false;  // Block member form: not rewritable in place.
    i += count;
  }

  auto slots_for = [&](uint32_t variable_id) -> uint32_t {
    auto var = variable_type.find(variable_id);
    if (var == variable_type.end()) return 8;
    auto ptr = pointer_pointee.find(var->second);
    if (ptr == pointer_pointee.end()) return 1;
    auto arr = array_length_id.find(ptr->second);
    if (arr == array_length_id.end()) return 1;
    auto len = constant_value.find(arr->second);
    return len == constant_value.end() ? 8 : std::max<uint32_t>(len->second, 1);
  };

  std::vector<uint32_t> out;
  out.reserve(words.size());
  out.insert(out.end(), words.begin(), words.begin() + 5);
  for (size_t i = 5; i < words.size();) {
    const uint32_t count = words[i] >> 16, op = words[i] & 0xFFFF;
    if (op == kOpCapability && count == 2 && words[i + 1] == kCapabilityClipDistance) {
      i += count;  // Drop the capability.
      continue;
    }
    const size_t start = out.size();
    out.insert(out.end(), words.begin() + i, words.begin() + i + count);
    if (op == kOpDecorate && count == 4 && words[i + 2] == kDecorationBuiltIn &&
        words[i + 3] == kBuiltInClipDistance) {
      out[start + 2] = kDecorationLocation;
      out[start + 3] = next_location;
      next_location += slots_for(words[i + 1]);
    }
    i += count;
  }
  words.swap(out);
  return found;
}

// Explicitly accepting const uint32_t* to make sure attention is paid to the
// alignment where this is called for safety on different host architectures.
inline VkShaderModule CreateShaderModule(const VulkanDevice* const vulkan_device,
                                         const uint32_t* const code, const size_t code_size_bytes) {
  VkShaderModuleCreateInfo shader_module_create_info;
  shader_module_create_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  shader_module_create_info.pNext = nullptr;
  shader_module_create_info.flags = 0;
  shader_module_create_info.codeSize = code_size_bytes;
  shader_module_create_info.pCode = code;
  std::vector<uint32_t> patched;
  if (!vulkan_device->properties().shaderClipDistance &&
      SpirvUsesClipDistance(code, code_size_bytes / sizeof(uint32_t))) {
    patched.assign(code, code + code_size_bytes / sizeof(uint32_t));
    if (StripSpirvClipDistance(patched)) {
      shader_module_create_info.codeSize = patched.size() * sizeof(uint32_t);
      shader_module_create_info.pCode = patched.data();
    }
  }
  VkShaderModule shader_module;
  return vulkan_device->functions().vkCreateShaderModule(vulkan_device->device(),
                                                         &shader_module_create_info, nullptr,
                                                         &shader_module) == VK_SUCCESS
             ? shader_module
             : VK_NULL_HANDLE;
}

VkPipeline CreateComputePipeline(const VulkanDevice* vulkan_device, VkPipelineLayout layout,
                                 VkShaderModule shader,
                                 const VkSpecializationInfo* specialization_info = nullptr,
                                 const char* entry_point = "main");
VkPipeline CreateComputePipeline(const VulkanDevice* vulkan_device, VkPipelineLayout layout,
                                 const uint32_t* shader_code, size_t shader_code_size_bytes,
                                 const VkSpecializationInfo* specialization_info = nullptr,
                                 const char* entry_point = "main");

}  // namespace util
}  // namespace vulkan
}  // namespace ui
}  // namespace rex
