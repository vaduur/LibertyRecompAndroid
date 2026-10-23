/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <cstdlib>
#include <atomic>
#if defined(__ANDROID__)
#include <android/log.h>
#endif
#include <algorithm>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/assert.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/string.h>
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/native_queue_policy.h>

#include "graphics/gta4_native/temporal/dlss_bootstrap.h"

REXCVAR_DEFINE_BOOL(vulkan_command_counters, false, "UI/Vulkan",
                    "Count barriers, renderings, draws, copies and pipeline binds per frame for "
                    "the pipeline report (a wrapper on every such command)");
REXCVAR_DEFINE_BOOL(vulkan_native_queue_separation, true, "UI/Vulkan",
                    "Isolate native offscreen work from drawable-dependent presentation")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(vulkan_require_fragment_stores_and_atomics, true, "UI/Vulkan",
                    "Deprecated and ignored for parity; fragmentStoresAndAtomics is always "
                    "required for Vulkan GPU emulation")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(vulkan_require_vertex_pipeline_stores_and_atomics, true, "UI/Vulkan",
                    "Deprecated and ignored for parity; vertexPipelineStoresAndAtomics is always "
                    "required for Vulkan GPU emulation")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// Apple Silicon / MoltenVK does not expose geometryShader or fillModeNonSolid;
// default both to false on macOS so the device can still be selected.
REXCVAR_DEFINE_BOOL(vulkan_require_geometry_shader, !REX_PLATFORM_MAC, "UI/Vulkan",
                    "Require geometryShader support for Vulkan GPU emulation (disable to allow "
                    "fallback primitive emulation paths)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(vulkan_require_fill_mode_non_solid, !REX_PLATFORM_MAC, "UI/Vulkan",
                    "Require fillModeNonSolid support for Vulkan GPU emulation (disable to "
                    "allow fallback to solid fill for line/point polygon modes)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace rex {
namespace ui {
namespace vulkan {

#include "command_counters.inc"

namespace {

// The feature the last device creation failed on. The first failure wins: the
// requirements are checked in order of how fundamental they are, so the
// earliest one is the one worth telling the user about.
std::string g_device_requirement_failure;

void RecordDeviceRequirementFailure(const char* unmet) {
  if (g_device_requirement_failure.empty()) {
    g_device_requirement_failure = unmet;
  }
}

}  // namespace

const std::string& LastDeviceRequirementFailure() { return g_device_requirement_failure; }

template <typename Structure, VkStructureType StructureType>
struct VulkanFeatures {
  Structure supported = {StructureType};
  Structure enabled = {StructureType};

  void Link(VkPhysicalDeviceFeatures2& supported_features_2,
            VkDeviceCreateInfo& device_create_info) {
    supported.pNext = supported_features_2.pNext;
    supported_features_2.pNext = &supported;
    enabled.pNext = const_cast<void*>(device_create_info.pNext);
    device_create_info.pNext = &enabled;
  }
};

std::unique_ptr<VulkanDevice> VulkanDevice::CreateIfSupported(
    const VulkanInstance* const vulkan_instance, const VkPhysicalDevice physical_device,
    const bool with_gpu_emulation, const bool with_swapchain,
    const bool with_native_shader_support, const bool with_dynamic_rendering,
    VulkanDeviceRequirements* const requirements) {
  assert_not_null(vulkan_instance);
  assert_not_null(physical_device);

  // The strict set, unless the caller says which ones it can do without.
  VulkanDeviceRequirements needed;
  if (requirements) {
    needed = *requirements;
  }

  const VulkanInstance::Functions& ifn = vulkan_instance->functions();

  // Get supported Vulkan 1.0 properties and features.

  VkPhysicalDeviceProperties properties = {};
  ifn.vkGetPhysicalDeviceProperties(physical_device, &properties);

  const uint32_t unclamped_api_version = properties.apiVersion;
  if (vulkan_instance->api_version() < VK_MAKE_API_VERSION(0, 1, 1, 0)) {
    // From the VkApplicationInfo specification:
    //
    // "The Khronos validation layers will treat apiVersion as the highest API
    // version the application targets, and will validate API usage against the
    // minimum of that version and the implementation version (instance or
    // device, depending on context). If an application tries to use
    // functionality from a greater version than this, a validation error will
    // be triggered."
    //
    // "Vulkan 1.0 implementations were required to return
    // VK_ERROR_INCOMPATIBLE_DRIVER if apiVersion was larger than 1.0."
    properties.apiVersion =
        VK_MAKE_API_VERSION(0, 1, 0, VK_API_VERSION_PATCH(properties.apiVersion));
  }

  VkPhysicalDeviceFeatures supported_features = {};
  ifn.vkGetPhysicalDeviceFeatures(physical_device, &supported_features);

  if (with_native_shader_support && needed.vulkan_1_2 &&
      properties.apiVersion < VK_MAKE_API_VERSION(0, 1, 2, 0)) {
    REXLOG_WARN(
        "Vulkan device '{}' exposes API {}.{}, but Liberty native shaders require Vulkan 1.2",
        properties.deviceName, VK_VERSION_MAJOR(properties.apiVersion),
        VK_VERSION_MINOR(properties.apiVersion));
    RecordDeviceRequirementFailure("vulkan_1_2");
    needed.unmet = "vulkan_1_2";
    if (requirements) {
      *requirements = needed;
    }
    return nullptr;
  }
  if (with_native_shader_support && needed.shader_int64 && !supported_features.shaderInt64) {
    REXLOG_WARN(
        "Vulkan device '{}' doesn't support shaderInt64 required by Liberty native shaders",
        properties.deviceName);
    RecordDeviceRequirementFailure("shaderInt64");
    needed.unmet = "shaderInt64";
    if (requirements) {
      *requirements = needed;
    }
    return nullptr;
  }
  if (with_native_shader_support && needed.depth_clamp && !supported_features.depthClamp) {
    REXLOG_WARN(
        "Vulkan device '{}' doesn't support depthClamp required by Liberty native rendering",
        properties.deviceName);
    RecordDeviceRequirementFailure("depthClamp");
    needed.unmet = "depthClamp";
    if (requirements) {
      *requirements = needed;
    }
    return nullptr;
  }
  if (with_native_shader_support && needed.sample_rate_shading && !supported_features.sampleRateShading) {
    // Declared statically by the per-sample fullscreen conversion shaders. A
    // draw-time flag cannot make that shader interface legal without it.
    REXLOG_WARN("Vulkan device '{}' lacks native shader capability sampleRateShading",
                properties.deviceName);
    RecordDeviceRequirementFailure("sampleRateShading");
    needed.unmet = "sampleRateShading";
    if (requirements) {
      *requirements = needed;
    }
    return nullptr;
  }
  if (with_native_shader_support && !supported_features.shaderClipDistance) {
    // The cached title vertex shaders declare ClipDistance. Without it (ARM
    // Mali) util::CreateShaderModule turns the clip output into an unread
    // ordinary output, so title clip planes stop clipping.
    REXLOG_WARN(
        "Vulkan device '{}' lacks shaderClipDistance; title clip planes are not applied",
        properties.deviceName);
  }

  if ((with_gpu_emulation || with_native_shader_support) &&
      needed.independent_blend && !supported_features.independentBlend) {
    // Independent per-target blend equations and write masks cannot be
    // represented faithfully without this feature.
    REXLOG_WARN(
        "Vulkan device '{}' doesn't support the independentBlend feature "
        "required for GPU emulation or Liberty native rendering",
        properties.deviceName);
    RecordDeviceRequirementFailure("independentBlend");
    needed.unmet = "independentBlend";
    if (requirements) {
      *requirements = needed;
    }
    return nullptr;
  }

  if (with_gpu_emulation) {
    if (!supported_features.fragmentStoresAndAtomics) {
      REXLOG_WARN(
          "Vulkan device '{}' doesn't support fragmentStoresAndAtomics, which "
          "is required for Vulkan GPU emulation parity",
          properties.deviceName);
      return nullptr;
    }
    if (!supported_features.vertexPipelineStoresAndAtomics) {
      REXLOG_WARN(
          "Vulkan device '{}' doesn't support vertexPipelineStoresAndAtomics, which "
          "is required for Vulkan GPU emulation parity",
          properties.deviceName);
      return nullptr;
    }
    if (REXCVAR_GET(vulkan_require_geometry_shader) && !supported_features.geometryShader) {
      REXLOG_WARN(
          "Vulkan device '{}' doesn't support geometryShader, but "
          "vulkan_require_geometry_shader=true",
          properties.deviceName);
      return nullptr;
    }
    if (REXCVAR_GET(vulkan_require_fill_mode_non_solid) && !supported_features.fillModeNonSolid) {
      REXLOG_WARN(
          "Vulkan device '{}' doesn't support fillModeNonSolid, but "
          "vulkan_require_fill_mode_non_solid=true",
          properties.deviceName);
      return nullptr;
    }
  }

  // Enable needed extensions.

  std::unique_ptr<VulkanDevice> device(new VulkanDevice(vulkan_instance, physical_device));

  const bool get_physical_device_properties2_supported =
      vulkan_instance->extensions().ext_1_1_KHR_get_physical_device_properties2;

  // Name pointers from `requested_extensions` will be used in the enabled
  // extensions vector.
  std::unordered_map<std::string, bool*> requested_extensions;

  const auto request_promoted_extension = [&](const char* const name, uint32_t const major,
                                              uint32_t const minor, bool* const supported_ptr) {
    assert_not_null(supported_ptr);
    if (properties.apiVersion >= VK_MAKE_API_VERSION(0, major, minor, 0)) {
      *supported_ptr = true;
    } else {
      requested_extensions.emplace(name, supported_ptr);
    }
  };

#define XE_UI_VULKAN_STRUCT_EXTENSION(name) \
  requested_extensions.emplace("VK_" #name, &device->extensions_.ext_##name);
#define XE_UI_VULKAN_LOCAL_EXTENSION(name) requested_extensions.emplace("VK_" #name, &ext_##name);
#define XE_UI_VULKAN_STRUCT_PROMOTED_EXTENSION(name, major, minor) \
  request_promoted_extension("VK_" #name, major, minor,            \
                             &device->extensions_.ext_##major##_##minor##_##name);
#define XE_UI_VULKAN_LOCAL_PROMOTED_EXTENSION(name, major, minor) \
  request_promoted_extension("VK_" #name, major, minor, &ext_##major##_##minor##_##name);

  bool ext_KHR_portability_subset = false;
  bool ext_1_2_KHR_driver_properties = false;
  if (get_physical_device_properties2_supported) {
    // #164. Must be enabled according to the specification if the physical
    // device is a portability subset one.
    XE_UI_VULKAN_LOCAL_EXTENSION(KHR_portability_subset)
    // #197
    XE_UI_VULKAN_LOCAL_PROMOTED_EXTENSION(KHR_driver_properties, 1, 2)
  }

  // Used by the Vulkan Memory Allocator and potentially by Xenia.
  // #128.
  XE_UI_VULKAN_STRUCT_PROMOTED_EXTENSION(KHR_dedicated_allocation, 1, 1)
  // #147. Also must be enabled for VK_KHR_dedicated_allocation and
  // VK_KHR_sampler_ycbcr_conversion.
  XE_UI_VULKAN_STRUCT_PROMOTED_EXTENSION(KHR_get_memory_requirements2, 1, 1)
  // #158. Also must be enabled for VK_KHR_sampler_ycbcr_conversion.
  XE_UI_VULKAN_STRUCT_PROMOTED_EXTENSION(KHR_bind_memory2, 1, 1)
  if (get_physical_device_properties2_supported) {
    // #238.
    XE_UI_VULKAN_STRUCT_EXTENSION(EXT_memory_budget)
  }
  if (properties.apiVersion >= VK_MAKE_API_VERSION(0, 1, 1, 0)) {
    // #414.
    XE_UI_VULKAN_STRUCT_PROMOTED_EXTENSION(KHR_maintenance4, 1, 3)
    // #55.
    XE_UI_VULKAN_STRUCT_PROMOTED_EXTENSION(KHR_dynamic_rendering, 1, 3)
  }

  // TEMP (Android perf investigation): hardware counters through
  // VK_KHR_performance_query when the driver offers them.
  bool ext_KHR_performance_query = false;
  bool ext_KHR_pipeline_executable_properties = false;
#if defined(__ANDROID__)
  if (get_physical_device_properties2_supported) {
    XE_UI_VULKAN_LOCAL_EXTENSION(KHR_performance_query)
    XE_UI_VULKAN_LOCAL_EXTENSION(KHR_pipeline_executable_properties)
  }
#endif
  // GPU breadcrumbs for hang diagnosis (gta4_native_gpu_breadcrumbs).
  XE_UI_VULKAN_STRUCT_EXTENSION(AMD_buffer_marker)
  if (with_swapchain) {
    // #2.
    XE_UI_VULKAN_STRUCT_EXTENSION(KHR_swapchain)
    XE_UI_VULKAN_STRUCT_EXTENSION(GOOGLE_display_timing)
  }

  bool ext_1_2_KHR_sampler_mirror_clamp_to_edge = false;
  bool ext_1_1_KHR_maintenance1 = false;
  bool ext_1_2_KHR_shader_float_controls = false;
  bool ext_EXT_fragment_shader_interlock = false;
  bool ext_1_3_EXT_shader_demote_to_helper_invocation = false;
  bool ext_EXT_non_seamless_cube_map = false;
  if (get_physical_device_properties2_supported &&
      (with_gpu_emulation || with_native_shader_support)) {
    XE_UI_VULKAN_STRUCT_EXTENSION(EXT_depth_clip_control)
  }
  if (with_gpu_emulation) {
    // #15.
    XE_UI_VULKAN_LOCAL_PROMOTED_EXTENSION(KHR_sampler_mirror_clamp_to_edge, 1, 2)
    // #70. Must be enabled for VK_KHR_sampler_ycbcr_conversion.
    XE_UI_VULKAN_LOCAL_PROMOTED_EXTENSION(KHR_maintenance1, 1, 1)
    // #141.
    XE_UI_VULKAN_STRUCT_EXTENSION(EXT_shader_stencil_export)
    // #148.
    XE_UI_VULKAN_STRUCT_PROMOTED_EXTENSION(KHR_image_format_list, 1, 2)
    if (get_physical_device_properties2_supported) {
      // #157.
      XE_UI_VULKAN_STRUCT_PROMOTED_EXTENSION(KHR_sampler_ycbcr_conversion, 1, 1)
      // #198. Also must be enabled for VK_KHR_spirv_1_4.
      XE_UI_VULKAN_LOCAL_PROMOTED_EXTENSION(KHR_shader_float_controls, 1, 2)
      // #252.
      XE_UI_VULKAN_LOCAL_EXTENSION(EXT_fragment_shader_interlock)
      // #277.
      XE_UI_VULKAN_LOCAL_PROMOTED_EXTENSION(EXT_shader_demote_to_helper_invocation, 1, 3)
      // #423.
      XE_UI_VULKAN_LOCAL_EXTENSION(EXT_non_seamless_cube_map)
      // Required for non-black YCbCr border color parity with D3D12.
      XE_UI_VULKAN_STRUCT_EXTENSION(EXT_custom_border_color)
      // Required for true null descriptors in bindless texture bindings.
      XE_UI_VULKAN_STRUCT_EXTENSION(EXT_robustness2)
    }
    if (properties.apiVersion >= VK_MAKE_API_VERSION(0, 1, 1, 0)) {
      // #237.
      XE_UI_VULKAN_STRUCT_PROMOTED_EXTENSION(KHR_spirv_1_4, 1, 2)
    }
  }
  if (!with_gpu_emulation && with_native_shader_support && get_physical_device_properties2_supported &&
      std::getenv("REX_VK_ROBUST") && std::getenv("REX_VK_ROBUST")[0] == '1') {
    XE_UI_VULKAN_STRUCT_EXTENSION(EXT_robustness2)
  }

#undef XE_UI_VULKAN_STRUCT_EXTENSION
#undef XE_UI_VULKAN_LOCAL_EXTENSION
#undef XE_UI_VULKAN_STRUCT_PROMOTED_EXTENSION
#undef XE_UI_VULKAN_LOCAL_PROMOTED_EXTENSION

  graphics::gta4_native::temporal::DlssExtensionRequirements dlss_requirements;
  if (vulkan_instance->dlss_extensions_enabled()) {
    dlss_requirements = graphics::gta4_native::temporal::QueryDlssDeviceExtensions(
        vulkan_instance->instance(), physical_device, properties.vendorID);
  } else {
    dlss_requirements.reason = vulkan_instance->dlss_unavailable_reason();
  }
  graphics::gta4_native::temporal::DlssExtensionRequirements dlss_fg_requirements;
  if (vulkan_instance->dlss_frame_generation_extensions_enabled()) {
    dlss_fg_requirements = graphics::gta4_native::temporal::QueryDlssDeviceExtensions(
        vulkan_instance->instance(), physical_device, properties.vendorID,
        graphics::gta4_native::temporal::DlssFeature::kFrameGeneration);
  } else {
    dlss_fg_requirements.reason = vulkan_instance->dlss_frame_generation_unavailable_reason();
  }
  std::map<std::string, bool> dlss_extension_states;
  for (const auto* requirements : {&dlss_requirements, &dlss_fg_requirements}) {
    for (const auto& name : requirements->extensions) {
      auto state = dlss_extension_states.emplace(name, false).first;
      requested_extensions.try_emplace(name, &state->second);
    }
  }

  std::vector<const char*> enabled_extensions;
  {
    uint32_t supported_extension_count = 0;
    const VkResult get_supported_extension_count_result = ifn.vkEnumerateDeviceExtensionProperties(
        physical_device, nullptr, &supported_extension_count, nullptr);
    if (get_supported_extension_count_result != VK_SUCCESS &&
        get_supported_extension_count_result != VK_INCOMPLETE) {
      REXLOG_WARN("Failed to get the Vulkan device '{}' extension count", properties.deviceName);
      return nullptr;
    }
    if (supported_extension_count) {
      std::vector<VkExtensionProperties> supported_extensions(supported_extension_count);
      if (ifn.vkEnumerateDeviceExtensionProperties(physical_device, nullptr,
                                                   &supported_extension_count,
                                                   supported_extensions.data()) != VK_SUCCESS) {
        REXLOG_WARN("Failed to get the Vulkan device '{}' extensions", properties.deviceName);
        return nullptr;
      }
      assert_true(supported_extension_count == supported_extensions.size());
#if defined(__ANDROID__)
      {
        std::string names;
        for (const VkExtensionProperties& e : supported_extensions) names += std::string(e.extensionName) + " ";
        for (size_t offset = 0; offset < names.size(); offset += 900)
          __android_log_print(ANDROID_LOG_WARN, "LibertyRecomp", "device-extensions: %s",
                              names.substr(offset, 900).c_str());
      }
#endif
      for (const VkExtensionProperties& supported_extension : supported_extensions) {
        const auto requested_extension_it =
            requested_extensions.find(supported_extension.extensionName);
        if (requested_extension_it == requested_extensions.cend()) {
          continue;
        }
        assert_not_null(requested_extension_it->second);
        if (!*requested_extension_it->second) {
          enabled_extensions.emplace_back(requested_extension_it->first.c_str());
          *requested_extension_it->second = true;
        }
      }
    }
  }

  const auto check_dlss_extensions = [&](const auto& requirements, bool& enabled,
                                          std::string& reason) {
    enabled = requirements.available;
    reason = requirements.reason;
    for (const auto& name : requirements.extensions) {
      if (!*requested_extensions.at(name)) {
        enabled = false;
        reason = "Missing required DLSS device extension: " + name;
        break;
      }
    }
  };
  check_dlss_extensions(dlss_requirements, device->dlss_extensions_enabled_,
                         device->dlss_unavailable_reason_);
  check_dlss_extensions(dlss_fg_requirements, device->dlss_fg_extensions_enabled_,
                         device->dlss_fg_unavailable_reason_);

  if (with_swapchain && !device->extensions_.ext_KHR_swapchain) {
    REXLOG_WARN("Vulkan device '{}' doesn't support swapchains", properties.deviceName);
    return nullptr;
  }

  VkDeviceCreateInfo device_create_info = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};

  device_create_info.enabledExtensionCount = uint32_t(enabled_extensions.size());
  device_create_info.ppEnabledExtensionNames = enabled_extensions.data();

  // Get supported Vulkan 1.1+ and extension properties and features.
  //
  // The property and feature structures are initialized to zero or to the
  // minimum / maximum requirements for the simplicity of handling unavailable
  // VK_KHR_get_physical_device_properties2.

  VkPhysicalDeviceProperties2 properties_2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};

  VkPhysicalDeviceFeatures2 supported_features_2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};

  VulkanFeatures<VkPhysicalDeviceVulkan12Features,
                 VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES>
      features_1_2;
  VkPhysicalDeviceVulkan12Properties properties_1_2 = {
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
  VulkanFeatures<VkPhysicalDeviceVulkan13Features,
                 VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES>
      features_1_3;
  VulkanFeatures<VkPhysicalDevicePortabilitySubsetFeaturesKHR,
                 VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PORTABILITY_SUBSET_FEATURES_KHR>
      features_KHR_portability_subset;
  VkPhysicalDevicePortabilitySubsetPropertiesKHR properties_KHR_portability_subset = {
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PORTABILITY_SUBSET_PROPERTIES_KHR};
  VkPhysicalDeviceDriverPropertiesKHR properties_1_2_KHR_driver_properties = {
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
  VkPhysicalDeviceFloatControlsProperties properties_1_2_KHR_shader_float_controls = {
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT_CONTROLS_PROPERTIES};
  VulkanFeatures<VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT,
                 VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT>
      features_EXT_fragment_shader_interlock;
  VulkanFeatures<VkPhysicalDeviceDepthClipControlFeaturesEXT,
                 VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT>
      features_EXT_depth_clip_control;
  VulkanFeatures<VkPhysicalDeviceDynamicRenderingFeaturesKHR,
                 VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR>
      features_1_3_KHR_dynamic_rendering;
  VulkanFeatures<VkPhysicalDeviceShaderDemoteToHelperInvocationFeaturesEXT,
                 VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DEMOTE_TO_HELPER_INVOCATION_FEATURES_EXT>
      features_1_3_EXT_shader_demote_to_helper_invocation;
  VulkanFeatures<VkPhysicalDeviceNonSeamlessCubeMapFeaturesEXT,
                 VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_NON_SEAMLESS_CUBE_MAP_FEATURES_EXT>
      features_EXT_non_seamless_cube_map;
  VulkanFeatures<VkPhysicalDeviceCustomBorderColorFeaturesEXT,
                 VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT>
      features_EXT_custom_border_color;
  VkPhysicalDeviceCustomBorderColorPropertiesEXT properties_EXT_custom_border_color = {
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_PROPERTIES_EXT};
  VulkanFeatures<VkPhysicalDeviceRobustness2FeaturesEXT,
                 VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT>
      features_EXT_robustness2;
  VulkanFeatures<VkPhysicalDevicePerformanceQueryFeaturesKHR,
                 VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PERFORMANCE_QUERY_FEATURES_KHR>
      features_KHR_performance_query;
  VulkanFeatures<VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR,
                 VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR>
      features_KHR_pipeline_executable_properties;

  if (get_physical_device_properties2_supported) {
    if (properties.apiVersion >= VK_MAKE_API_VERSION(0, 1, 2, 0)) {
      features_1_2.Link(supported_features_2, device_create_info);
      properties_1_2.pNext = properties_2.pNext;
      properties_2.pNext = &properties_1_2;
    }
    if (properties.apiVersion >= VK_MAKE_API_VERSION(0, 1, 3, 0)) {
      features_1_3.Link(supported_features_2, device_create_info);
    } else {
      if (device->extensions_.ext_1_3_KHR_dynamic_rendering) {
        features_1_3_KHR_dynamic_rendering.Link(supported_features_2, device_create_info);
      }
      if (ext_1_3_EXT_shader_demote_to_helper_invocation) {
        features_1_3_EXT_shader_demote_to_helper_invocation.Link(supported_features_2,
                                                                 device_create_info);
      }
    }
    if (ext_KHR_portability_subset) {
      features_KHR_portability_subset.Link(supported_features_2, device_create_info);
      properties_KHR_portability_subset.pNext = properties_2.pNext;
      properties_2.pNext = &properties_KHR_portability_subset;
    }
    if (ext_1_2_KHR_driver_properties) {
      properties_1_2_KHR_driver_properties.pNext = properties_2.pNext;
      properties_2.pNext = &properties_1_2_KHR_driver_properties;
    }
    if (ext_1_2_KHR_shader_float_controls) {
      properties_1_2_KHR_shader_float_controls.pNext = properties_2.pNext;
      properties_2.pNext = &properties_1_2_KHR_shader_float_controls;
    }
    if (ext_EXT_fragment_shader_interlock) {
      features_EXT_fragment_shader_interlock.Link(supported_features_2, device_create_info);
    }
    if (device->extensions_.ext_EXT_depth_clip_control) {
      features_EXT_depth_clip_control.Link(supported_features_2, device_create_info);
    }
    if (ext_EXT_non_seamless_cube_map) {
      features_EXT_non_seamless_cube_map.Link(supported_features_2, device_create_info);
    }
    if (device->extensions_.ext_EXT_custom_border_color) {
      features_EXT_custom_border_color.Link(supported_features_2, device_create_info);
      properties_EXT_custom_border_color.pNext = properties_2.pNext;
      properties_2.pNext = &properties_EXT_custom_border_color;
    }
    if (device->extensions_.ext_EXT_robustness2) {
      features_EXT_robustness2.Link(supported_features_2, device_create_info);
    }
    if (ext_KHR_performance_query) {
      features_KHR_performance_query.Link(supported_features_2, device_create_info);
    }
    if (ext_KHR_pipeline_executable_properties) {
      features_KHR_pipeline_executable_properties.Link(supported_features_2, device_create_info);
    }
    ifn.vkGetPhysicalDeviceProperties2(physical_device, &properties_2);
    ifn.vkGetPhysicalDeviceFeatures2(physical_device, &supported_features_2);
  }

  if (with_native_shader_support &&
      (!features_1_2.supported.runtimeDescriptorArray ||
       !features_1_2.supported.descriptorBindingPartiallyBound ||
       !features_1_2.supported.bufferDeviceAddress)) {
    REXLOG_WARN(
        "Vulkan device '{}' lacks native shader features: runtimeDescriptorArray={}, "
        "descriptorBindingPartiallyBound={}, bufferDeviceAddress={}",
        properties.deviceName, features_1_2.supported.runtimeDescriptorArray != VK_FALSE,
        features_1_2.supported.descriptorBindingPartiallyBound != VK_FALSE,
        features_1_2.supported.bufferDeviceAddress != VK_FALSE);
    return nullptr;
  }
  if (with_dynamic_rendering) {
    const bool dynamic_rendering_supported =
        properties.apiVersion >= VK_MAKE_API_VERSION(0, 1, 3, 0)
            ? features_1_3.supported.dynamicRendering != VK_FALSE
            : device->extensions_.ext_1_3_KHR_dynamic_rendering &&
                  features_1_3_KHR_dynamic_rendering.supported.dynamicRendering != VK_FALSE;
    if (!dynamic_rendering_supported) {
      REXLOG_WARN("Vulkan device '{}' doesn't support dynamic rendering required by the native "
                  "graphics backend",
                  properties.deviceName);
      return nullptr;
    }
  }

  uint32_t queue_family_count = 0;
  ifn.vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_family_count, nullptr);
  std::vector<VkQueueFamilyProperties> queue_families(queue_family_count);
  ifn.vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_family_count,
                                               queue_families.data());

  device->queue_families_.resize(queue_family_count);

  uint32_t first_queue_family_graphics_compute_sparse_binding = UINT32_MAX;
  uint32_t first_queue_family_graphics_compute = UINT32_MAX;
  uint32_t first_queue_family_sparse_binding = UINT32_MAX;
  bool has_presentation_queue_family = false;

  for (uint32_t queue_family_index = 0; queue_family_index < queue_family_count;
       ++queue_family_index) {
    QueueFamily& queue_family = device->queue_families_[queue_family_index];
    const VkQueueFamilyProperties& queue_family_properties = queue_families[queue_family_index];
    queue_family.queue_flags = queue_family_properties.queueFlags;

    const VkQueueFlags queue_unsupported_flags = ~queue_family_properties.queueFlags;

    if (!(queue_unsupported_flags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))) {
      first_queue_family_graphics_compute =
          std::min(queue_family_index, first_queue_family_graphics_compute);
    }

    if (with_gpu_emulation && supported_features.sparseBinding &&
        !(queue_unsupported_flags & VK_QUEUE_SPARSE_BINDING_BIT)) {
      first_queue_family_sparse_binding =
          std::min(queue_family_index, first_queue_family_sparse_binding);
      if (!(queue_unsupported_flags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))) {
        first_queue_family_graphics_compute_sparse_binding =
            std::min(queue_family_index, first_queue_family_graphics_compute_sparse_binding);
      }
    }

    if (with_swapchain) {
#if REX_PLATFORM_WIN32
      queue_family.may_support_presentation =
          vulkan_instance->extensions().ext_KHR_win32_surface &&
          ifn.vkGetPhysicalDeviceWin32PresentationSupportKHR(physical_device, queue_family_index);
#else
      queue_family.may_support_presentation = true;
#endif
      if (queue_family.may_support_presentation) {
        queue_family.queues.resize(std::max(size_t(1), queue_family.queues.size()));
        has_presentation_queue_family = true;
      }
    }
  }

  if (first_queue_family_graphics_compute == UINT32_MAX) {
    // Not valid according to the Vulkan specification, but for safety.
    REXLOG_WARN(
        "Vulkan device '{}' doesn't provide a graphics and compute queue "
        "family",
        properties.deviceName);
    return nullptr;
  }

  if (with_swapchain && !has_presentation_queue_family) {
    REXLOG_WARN(
        "Vulkan device '{}' doesn't provide a queue family that supports "
        "presentation",
        properties.deviceName);
    return nullptr;
  }

  // Get the queues to create.

  if (first_queue_family_sparse_binding == UINT32_MAX) {
    // Not valid not to provide a sparse binding queue if the sparseBinding
    // feature is supported according to the Vulkan specification, but for
    // safety and simplicity.
    supported_features.sparseBinding = VK_FALSE;
  }
  if (!supported_features.sparseBinding) {
    supported_features.sparseResidencyBuffer = VK_FALSE;
    supported_features.sparseResidencyImage2D = VK_FALSE;
    supported_features.sparseResidencyImage3D = VK_FALSE;
    supported_features.sparseResidency2Samples = VK_FALSE;
    supported_features.sparseResidency4Samples = VK_FALSE;
    supported_features.sparseResidency8Samples = VK_FALSE;
    supported_features.sparseResidency16Samples = VK_FALSE;
    supported_features.sparseResidencyAliased = VK_FALSE;
  }

  // The original graphics queue remains the UI/presentation queue. Native
  // offscreen rendering can use a separate backend execution queue below.

  if (first_queue_family_graphics_compute_sparse_binding != UINT32_MAX) {
    device->queue_family_graphics_compute_ = first_queue_family_graphics_compute_sparse_binding;
    device->queue_family_sparse_binding_ = first_queue_family_graphics_compute_sparse_binding;
  } else {
    device->queue_family_graphics_compute_ = first_queue_family_graphics_compute;
    device->queue_family_sparse_binding_ = first_queue_family_sparse_binding;
  }

  device->queue_families_[device->queue_family_graphics_compute_].queues.resize(std::max(
      size_t(1), device->queue_families_[device->queue_family_graphics_compute_].queues.size()));
  if (device->queue_family_sparse_binding_ != UINT32_MAX) {
    device->queue_families_[device->queue_family_sparse_binding_].queues.resize(std::max(
        size_t(1), device->queue_families_[device->queue_family_sparse_binding_].queues.size()));
  }

  device->queue_family_native_offscreen_ = SelectNativeOffscreenQueueFamily(
      queue_families, device->queue_family_graphics_compute_,
      with_native_shader_support && with_swapchain && !with_gpu_emulation &&
          REXCVAR_GET(vulkan_native_queue_separation),
      properties.apiVersion >= VK_MAKE_API_VERSION(0, 1, 2, 0) &&
          features_1_2.supported.timelineSemaphore,
      properties_1_2.driverID == VK_DRIVER_ID_MOLTENVK ||
          properties_1_2_KHR_driver_properties.driverID == VK_DRIVER_ID_MOLTENVK);
  if (device->has_native_offscreen_queue()) {
    device->queue_families_[device->queue_family_native_offscreen_].queues.resize(1);
    features_1_2.enabled.timelineSemaphore = VK_TRUE;
    device->properties_.timelineSemaphore = true;
  }
  REXLOG_INFO("Vulkan native queues: scene-family={} paint-family={} separated={}",
              device->queue_family_native_offscreen_, device->queue_family_graphics_compute_,
              device->has_native_offscreen_queue());

  size_t max_enabled_queues_per_family = 0;
  for (const QueueFamily& queue_family : device->queue_families_) {
    max_enabled_queues_per_family =
        std::max(queue_family.queues.size(), max_enabled_queues_per_family);
  }
  const std::vector<float> queue_priorities(max_enabled_queues_per_family, 1.0f);
  std::vector<VkDeviceQueueCreateInfo> queue_create_infos;
  for (size_t queue_family_index = 0; queue_family_index < device->queue_families_.size();
       ++queue_family_index) {
    const QueueFamily& queue_family = device->queue_families_[queue_family_index];
    if (queue_family.queues.empty()) {
      continue;
    }
    VkDeviceQueueCreateInfo& queue_create_info = queue_create_infos.emplace_back();
    queue_create_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_create_info.pNext = nullptr;
    queue_create_info.flags = 0;
    queue_create_info.queueFamilyIndex = uint32_t(queue_family_index);
    queue_create_info.queueCount = uint32_t(queue_family.queues.size());
    queue_create_info.pQueuePriorities = queue_priorities.data();
  }
  device_create_info.queueCreateInfoCount = uint32_t(queue_create_infos.size());
  device_create_info.pQueueCreateInfos = queue_create_infos.data();

  // Enable needed features and copy the properties.
  //
  // Enabling only actually used features because drivers may take more optimal
  // paths when certain features are disabled. Also, in VK_EXT_shader_object,
  // the state that the application must set for the draw depends on which
  // features are enabled.

  device->properties_.apiVersion = properties.apiVersion;
  device->properties_.driverVersion = properties.driverVersion;
  device->properties_.vendorID = properties.vendorID;
  device->properties_.deviceID = properties.deviceID;
  rex::string::copy_truncating(device->properties_.deviceName, properties.deviceName,
                               sizeof(device->properties_.deviceName));

  REXLOG_INFO(
      "Vulkan device '{}': API {}.{}.{}, vendor 0x{:04X}, device 0x{:04X}, "
      "driver version 0x{:X}",
      properties.deviceName, VK_VERSION_MAJOR(properties.apiVersion),
      VK_VERSION_MINOR(properties.apiVersion), VK_VERSION_PATCH(properties.apiVersion),
      properties.vendorID, properties.deviceID, properties.driverVersion);
  if (unclamped_api_version != properties.apiVersion) {
    REXLOG_INFO(
        "Device supports Vulkan API {}.{}.{}, but the used version is limited "
        "by the instance",
        VK_VERSION_MAJOR(unclamped_api_version), VK_VERSION_MINOR(unclamped_api_version),
        VK_VERSION_PATCH(unclamped_api_version));
  }

  REXLOG_INFO("Enabled Vulkan device extensions:");
  for (uint32_t enabled_extension_index = 0;
       enabled_extension_index < device_create_info.enabledExtensionCount;
       ++enabled_extension_index) {
    REXLOG_INFO("* {}", device_create_info.ppEnabledExtensionNames[enabled_extension_index]);
  }

  REXLOG_INFO("Vulkan device properties and enabled features:");

  VkPhysicalDeviceFeatures enabled_features = {};
  device_create_info.pEnabledFeatures = &enabled_features;

#define XE_UI_VULKAN_LIMIT(name)                     \
  device->properties_.name = properties.limits.name; \
  REXLOG_INFO("* " #name ": {}", properties.limits.name);
#define XE_UI_VULKAN_ENUM_LIMIT(name, type)          \
  device->properties_.name = properties.limits.name; \
  REXLOG_INFO("* " #name ": {}", vk::to_string(vk::type(properties.limits.name)));
#define XE_UI_VULKAN_FEATURE(name)                    \
  enabled_features.name = supported_features.name;    \
  device->properties_.name = supported_features.name; \
  if (supported_features.name) {                      \
    REXLOG_INFO("* " #name);                          \
  }
#define XE_UI_VULKAN_PROPERTY_2(structure, name) \
  device->properties_.name = structure.name;     \
  REXLOG_INFO("* " #name ": {}", structure.name);
#define XE_UI_VULKAN_ENUM_PROPERTY_2(structure, name, type) \
  device->properties_.name = structure.name;                \
  REXLOG_INFO("* " #name ": {}", vk::to_string(vk::type(structure.name)));
#define XE_UI_VULKAN_FEATURE_2(structure, name)        \
  structure.enabled.name = structure.supported.name;   \
  device->properties_.name = structure.supported.name; \
  if (structure.supported.name) {                      \
    REXLOG_INFO("* " #name);                           \
  }
#define XE_UI_VULKAN_FEATURE_IMPLIED(name) \
  device->properties_.name = true;         \
  REXLOG_INFO("* " #name);

  if (ext_1_2_KHR_driver_properties) {
    XE_UI_VULKAN_ENUM_PROPERTY_2(properties_1_2_KHR_driver_properties, driverID, DriverId);
    REXLOG_INFO("* driverName: {}", properties_1_2_KHR_driver_properties.driverName);
    if (properties_1_2_KHR_driver_properties.driverInfo[0]) {
      REXLOG_INFO("* driverInfo: {}", properties_1_2_KHR_driver_properties.driverInfo);
    }
    REXLOG_INFO("* conformanceVersion: {}.{}.{}.{}",
                properties_1_2_KHR_driver_properties.conformanceVersion.major,
                properties_1_2_KHR_driver_properties.conformanceVersion.minor,
                properties_1_2_KHR_driver_properties.conformanceVersion.subminor,
                properties_1_2_KHR_driver_properties.conformanceVersion.patch);
  }

  XE_UI_VULKAN_LIMIT(maxImageDimension2D)
  XE_UI_VULKAN_LIMIT(maxImageDimension3D)
  XE_UI_VULKAN_LIMIT(maxImageDimensionCube)
  XE_UI_VULKAN_LIMIT(maxImageArrayLayers)
  XE_UI_VULKAN_LIMIT(maxStorageBufferRange)
  XE_UI_VULKAN_LIMIT(maxSamplerAllocationCount)
  XE_UI_VULKAN_LIMIT(maxPerStageDescriptorSamplers)
  XE_UI_VULKAN_LIMIT(maxPerStageDescriptorStorageBuffers)
  XE_UI_VULKAN_LIMIT(maxPerStageDescriptorSampledImages)
  XE_UI_VULKAN_LIMIT(maxPerStageResources)
  XE_UI_VULKAN_LIMIT(maxDescriptorSetSamplers)
  XE_UI_VULKAN_LIMIT(maxDescriptorSetSampledImages)
  if (properties.apiVersion >= VK_MAKE_API_VERSION(0, 1, 2, 0)) {
    XE_UI_VULKAN_PROPERTY_2(properties_1_2, maxUpdateAfterBindDescriptorsInAllPools)
    XE_UI_VULKAN_PROPERTY_2(properties_1_2, maxPerStageDescriptorUpdateAfterBindSamplers)
    XE_UI_VULKAN_PROPERTY_2(properties_1_2, maxPerStageDescriptorUpdateAfterBindSampledImages)
    XE_UI_VULKAN_PROPERTY_2(properties_1_2, maxPerStageUpdateAfterBindResources)
    XE_UI_VULKAN_PROPERTY_2(properties_1_2, maxDescriptorSetUpdateAfterBindSamplers)
    XE_UI_VULKAN_PROPERTY_2(properties_1_2, maxDescriptorSetUpdateAfterBindSampledImages)
  }
  XE_UI_VULKAN_LIMIT(maxVertexOutputComponents)
  XE_UI_VULKAN_LIMIT(maxVertexInputBindings)
  XE_UI_VULKAN_LIMIT(maxVertexInputAttributes)
  XE_UI_VULKAN_LIMIT(maxVertexInputBindingStride)
  XE_UI_VULKAN_LIMIT(maxVertexInputAttributeOffset)
  XE_UI_VULKAN_LIMIT(maxTessellationEvaluationOutputComponents)
  XE_UI_VULKAN_LIMIT(maxGeometryInputComponents)
  XE_UI_VULKAN_LIMIT(maxGeometryOutputComponents)
  XE_UI_VULKAN_LIMIT(maxFragmentInputComponents)
  XE_UI_VULKAN_LIMIT(maxFragmentCombinedOutputResources)
  XE_UI_VULKAN_LIMIT(maxSamplerAnisotropy)
  XE_UI_VULKAN_LIMIT(maxSamplerLodBias)
  XE_UI_VULKAN_LIMIT(maxViewportDimensions[0])
  XE_UI_VULKAN_LIMIT(maxViewportDimensions[1])
  XE_UI_VULKAN_LIMIT(viewportBoundsRange[0])
  XE_UI_VULKAN_LIMIT(viewportBoundsRange[1])
  XE_UI_VULKAN_LIMIT(minUniformBufferOffsetAlignment)
  XE_UI_VULKAN_LIMIT(minStorageBufferOffsetAlignment)
  XE_UI_VULKAN_LIMIT(maxFramebufferWidth)
  XE_UI_VULKAN_LIMIT(maxFramebufferHeight)
  XE_UI_VULKAN_LIMIT(maxColorAttachments)
  XE_UI_VULKAN_ENUM_LIMIT(framebufferColorSampleCounts, SampleCountFlags)
  XE_UI_VULKAN_ENUM_LIMIT(framebufferDepthSampleCounts, SampleCountFlags)
  XE_UI_VULKAN_ENUM_LIMIT(framebufferStencilSampleCounts, SampleCountFlags)
  XE_UI_VULKAN_ENUM_LIMIT(framebufferNoAttachmentsSampleCounts, SampleCountFlags)
  XE_UI_VULKAN_ENUM_LIMIT(sampledImageColorSampleCounts, SampleCountFlags)
  XE_UI_VULKAN_ENUM_LIMIT(sampledImageIntegerSampleCounts, SampleCountFlags)
  XE_UI_VULKAN_ENUM_LIMIT(sampledImageDepthSampleCounts, SampleCountFlags)
  XE_UI_VULKAN_ENUM_LIMIT(sampledImageStencilSampleCounts, SampleCountFlags)
  XE_UI_VULKAN_LIMIT(standardSampleLocations)
  XE_UI_VULKAN_LIMIT(optimalBufferCopyOffsetAlignment)
  XE_UI_VULKAN_LIMIT(optimalBufferCopyRowPitchAlignment)
  XE_UI_VULKAN_LIMIT(nonCoherentAtomSize)

  // TEMP (Android GPU hang hunt): REX_VK_ROBUST=1 also enables bounds-checked
  // buffer access and null descriptors for the native GTA IV renderer.
  const bool native_robustness = with_native_shader_support && std::getenv("REX_VK_ROBUST") &&
                                 std::getenv("REX_VK_ROBUST")[0] == '1';
  if (native_robustness && !with_gpu_emulation) {
    XE_UI_VULKAN_FEATURE(robustBufferAccess)
  }
  if (with_gpu_emulation) {
    if (device->properties_.driverID != VK_DRIVER_ID_MOLTENVK) {
      XE_UI_VULKAN_FEATURE(robustBufferAccess)
    } else {
      device->properties_.robustBufferAccess = false;
      REXLOG_INFO("* robustBufferAccess omitted because MoltenVK cannot implement it on Metal");
    }
    XE_UI_VULKAN_FEATURE(fullDrawIndexUint32)
    XE_UI_VULKAN_FEATURE(geometryShader)
    XE_UI_VULKAN_FEATURE(tessellationShader)
    XE_UI_VULKAN_FEATURE(occlusionQueryPrecise)
    XE_UI_VULKAN_FEATURE(vertexPipelineStoresAndAtomics)
    XE_UI_VULKAN_FEATURE(fragmentStoresAndAtomics)
    XE_UI_VULKAN_FEATURE(shaderCullDistance)
    XE_UI_VULKAN_FEATURE(sparseBinding)
    XE_UI_VULKAN_FEATURE(sparseResidencyBuffer)
  }

  if (with_gpu_emulation || with_native_shader_support) {
    XE_UI_VULKAN_FEATURE(sampleRateShading)
    XE_UI_VULKAN_FEATURE(independentBlend)
    XE_UI_VULKAN_FEATURE(depthClamp)
    XE_UI_VULKAN_FEATURE(fillModeNonSolid)
    XE_UI_VULKAN_FEATURE(samplerAnisotropy)
    XE_UI_VULKAN_FEATURE(shaderClipDistance)
  }

  if (with_native_shader_support) {
    XE_UI_VULKAN_FEATURE(shaderInt64)
    // FidelityFX's Vulkan backend selects permutations from physical-device
    // capabilities. Enable the corresponding logical-device capabilities so
    // its Float16/Int16 and unformatted storage-image SPIR-V is legal.
    XE_UI_VULKAN_FEATURE(shaderInt16)
    XE_UI_VULKAN_FEATURE(shaderStorageImageExtendedFormats)
    XE_UI_VULKAN_FEATURE(shaderStorageImageReadWithoutFormat)
    XE_UI_VULKAN_FEATURE(shaderStorageImageWriteWithoutFormat)
  }

  if (properties.apiVersion >= VK_MAKE_API_VERSION(0, 1, 2, 0)) {
    if (with_gpu_emulation || with_native_shader_support) {
      XE_UI_VULKAN_FEATURE_2(features_1_2, samplerMirrorClampToEdge);
    }
    if (with_gpu_emulation) {
      XE_UI_VULKAN_FEATURE_2(features_1_2, uniformBufferStandardLayout);
      XE_UI_VULKAN_FEATURE_2(features_1_2, scalarBlockLayout);
    }
    if (with_native_shader_support) {
      XE_UI_VULKAN_FEATURE_2(features_1_2, shaderFloat16);
      XE_UI_VULKAN_FEATURE_2(features_1_2, runtimeDescriptorArray);
      XE_UI_VULKAN_FEATURE_2(features_1_2, descriptorBindingPartiallyBound);
      XE_UI_VULKAN_FEATURE_2(features_1_2, descriptorBindingSampledImageUpdateAfterBind);
      XE_UI_VULKAN_FEATURE_2(features_1_2, descriptorBindingUpdateUnusedWhilePending);
      // Vulkan uses descriptorBindingSampledImageUpdateAfterBind for sampler,
      // combined-image-sampler and sampled-image descriptors; there is no
      // separate VkPhysicalDeviceVulkan12Features sampler member.
      device->properties_.descriptorBindingSamplerUpdateAfterBind =
          features_1_2.supported.descriptorBindingSampledImageUpdateAfterBind;
      XE_UI_VULKAN_FEATURE_2(features_1_2, descriptorBindingVariableDescriptorCount);
      XE_UI_VULKAN_FEATURE_2(features_1_2, bufferDeviceAddress);
    }
  } else {
    if (ext_1_2_KHR_sampler_mirror_clamp_to_edge) {
      XE_UI_VULKAN_FEATURE_IMPLIED(samplerMirrorClampToEdge)
    }
  }

  if (properties.apiVersion >= VK_MAKE_API_VERSION(0, 1, 3, 0)) {
    if (with_gpu_emulation) {
      XE_UI_VULKAN_FEATURE_2(features_1_3, shaderDemoteToHelperInvocation);
    }
    if (with_gpu_emulation || with_dynamic_rendering) {
      XE_UI_VULKAN_FEATURE_2(features_1_3, dynamicRendering);
    }
  } else {
    if (device->extensions_.ext_1_3_KHR_dynamic_rendering) {
      if (with_gpu_emulation || with_dynamic_rendering) {
        XE_UI_VULKAN_FEATURE_2(features_1_3_KHR_dynamic_rendering, dynamicRendering);
      }
    }
    if (ext_1_3_EXT_shader_demote_to_helper_invocation) {
      if (with_gpu_emulation) {
        XE_UI_VULKAN_FEATURE_2(features_1_3_EXT_shader_demote_to_helper_invocation,
                               shaderDemoteToHelperInvocation);
      }
    }
  }

  if (ext_KHR_portability_subset) {
    device->properties_.portabilitySubset = true;
    XE_UI_VULKAN_PROPERTY_2(properties_KHR_portability_subset,
                           minVertexInputBindingStrideAlignment)
    if (with_gpu_emulation) {
      XE_UI_VULKAN_FEATURE_2(features_KHR_portability_subset, constantAlphaColorBlendFactors)
      XE_UI_VULKAN_FEATURE_2(features_KHR_portability_subset, imageViewFormatReinterpretation)
      XE_UI_VULKAN_FEATURE_2(features_KHR_portability_subset, imageViewFormatSwizzle)
      XE_UI_VULKAN_FEATURE_2(features_KHR_portability_subset, pointPolygons)
      XE_UI_VULKAN_FEATURE_2(features_KHR_portability_subset, separateStencilMaskRef)
      XE_UI_VULKAN_FEATURE_2(features_KHR_portability_subset,
                             shaderSampleRateInterpolationFunctions)
    } else if (with_native_shader_support) {
      // Feature enablement is separate from native ordinary-texture policy:
      // those views remain identity; vector-font atlases need swizzling.
      XE_UI_VULKAN_FEATURE_2(features_KHR_portability_subset, constantAlphaColorBlendFactors)
      XE_UI_VULKAN_FEATURE_2(features_KHR_portability_subset, imageViewFormatSwizzle)
      XE_UI_VULKAN_FEATURE_2(features_KHR_portability_subset, samplerMipLodBias)
      XE_UI_VULKAN_FEATURE_2(features_KHR_portability_subset, separateStencilMaskRef)
      XE_UI_VULKAN_FEATURE_2(features_KHR_portability_subset, triangleFans)
      XE_UI_VULKAN_FEATURE_2(features_KHR_portability_subset, vertexAttributeAccessBeyondStride)
    }
  } else {
    // Not a portability subset device.
    XE_UI_VULKAN_FEATURE_IMPLIED(constantAlphaColorBlendFactors)
    XE_UI_VULKAN_FEATURE_IMPLIED(imageViewFormatReinterpretation)
    XE_UI_VULKAN_FEATURE_IMPLIED(imageViewFormatSwizzle)
    XE_UI_VULKAN_FEATURE_IMPLIED(pointPolygons)
    XE_UI_VULKAN_FEATURE_IMPLIED(separateStencilMaskRef)
    XE_UI_VULKAN_FEATURE_IMPLIED(shaderSampleRateInterpolationFunctions)
    XE_UI_VULKAN_FEATURE_IMPLIED(samplerMipLodBias)
    XE_UI_VULKAN_FEATURE_IMPLIED(triangleFans)
    XE_UI_VULKAN_FEATURE_IMPLIED(vertexAttributeAccessBeyondStride)
  }

  if (ext_1_2_KHR_shader_float_controls) {
    XE_UI_VULKAN_PROPERTY_2(properties_1_2_KHR_shader_float_controls,
                            shaderSignedZeroInfNanPreserveFloat32);
    XE_UI_VULKAN_PROPERTY_2(properties_1_2_KHR_shader_float_controls,
                            shaderDenormFlushToZeroFloat32);
    XE_UI_VULKAN_PROPERTY_2(properties_1_2_KHR_shader_float_controls, shaderRoundingModeRTEFloat32);
  }

  if (ext_EXT_fragment_shader_interlock) {
    if (with_gpu_emulation) {
      XE_UI_VULKAN_FEATURE_2(features_EXT_fragment_shader_interlock, fragmentShaderSampleInterlock)
      XE_UI_VULKAN_FEATURE_2(features_EXT_fragment_shader_interlock, fragmentShaderPixelInterlock)
    }
  }

  if (device->extensions_.ext_EXT_depth_clip_control &&
      (with_gpu_emulation || with_native_shader_support)) {
    XE_UI_VULKAN_FEATURE_2(features_EXT_depth_clip_control, depthClipControl)
  }

  if (ext_EXT_non_seamless_cube_map) {
    if (with_gpu_emulation) {
      XE_UI_VULKAN_FEATURE_2(features_EXT_non_seamless_cube_map, nonSeamlessCubeMap)
    }
  }

  if (device->extensions_.ext_EXT_custom_border_color) {
    if (with_gpu_emulation) {
      XE_UI_VULKAN_FEATURE_2(features_EXT_custom_border_color, customBorderColors)
      XE_UI_VULKAN_FEATURE_2(features_EXT_custom_border_color, customBorderColorWithoutFormat)
    }
    XE_UI_VULKAN_PROPERTY_2(properties_EXT_custom_border_color, maxCustomBorderColorSamplers)
  }

  if (device->extensions_.ext_EXT_robustness2) {
    if (with_gpu_emulation || native_robustness) {
      XE_UI_VULKAN_FEATURE_2(features_EXT_robustness2, nullDescriptor)
    }
  }
  if (ext_KHR_performance_query) {
    features_KHR_performance_query.enabled.performanceCounterQueryPools =
        features_KHR_performance_query.supported.performanceCounterQueryPools;
    device->performance_query_enabled_ =
        features_KHR_performance_query.supported.performanceCounterQueryPools != VK_FALSE;
    REXLOG_INFO("* performanceCounterQueryPools: {}", device->performance_query_enabled_);
  }
  if (ext_KHR_pipeline_executable_properties) {
    features_KHR_pipeline_executable_properties.enabled.pipelineExecutableInfo =
        features_KHR_pipeline_executable_properties.supported.pipelineExecutableInfo;
    device->pipeline_statistics_enabled_ =
        features_KHR_pipeline_executable_properties.supported.pipelineExecutableInfo != VK_FALSE;
  }

#undef XE_UI_VULKAN_LIMIT
#undef XE_UI_VULKAN_ENUM_LIMIT
#undef XE_UI_VULKAN_FEATURE
#undef XE_UI_VULKAN_PROPERTY_2
#undef XE_UI_VULKAN_ENUM_PROPERTY_2
#undef XE_UI_VULKAN_FEATURE_2

  // Create the device.

  const VkResult device_create_result =
      ifn.vkCreateDevice(physical_device, &device_create_info, nullptr, &device->device_);
  if (device_create_result != VK_SUCCESS) {
    REXLOG_ERROR(
        "Failed to create a Vulkan logical device from the physical device "
        "'{}': {}",
        properties.deviceName, vk::to_string(vk::Result(device_create_result)));
    return nullptr;
  }

  // Load device functions.

  bool functions_loaded = true;

  Functions& dfn = device->functions_;

#define XE_UI_VULKAN_FUNCTION(name) \
  functions_loaded &=               \
      (dfn.name = PFN_##name(ifn.vkGetDeviceProcAddr(device->device_, #name))) != nullptr;

  // Vulkan 1.0.
#include <rex/ui/vulkan/functions/device_1_0.inc>

  // Extensions promoted to a Vulkan version supported by the device.
#define XE_UI_VULKAN_FUNCTION_PROMOTED(extension_name, core_name)                                \
  functions_loaded &=                                                                            \
      (dfn.core_name = PFN_##core_name(ifn.vkGetDeviceProcAddr(device->device_, #core_name))) != \
      nullptr;
  if (properties.apiVersion >= VK_MAKE_API_VERSION(0, 1, 1, 0)) {
#include <rex/ui/vulkan/functions/device_1_1_khr_bind_memory2.inc>
#include <rex/ui/vulkan/functions/device_1_1_khr_get_memory_requirements2.inc>
  }
  if (properties.apiVersion >= VK_MAKE_API_VERSION(0, 1, 2, 0)) {
#include <rex/ui/vulkan/functions/device_1_2.inc>
  }
  if (properties.apiVersion >= VK_MAKE_API_VERSION(0, 1, 3, 0)) {
#include <rex/ui/vulkan/functions/device_1_3_khr_dynamic_rendering.inc>
#include <rex/ui/vulkan/functions/device_1_3_khr_maintenance4.inc>
  }
#undef XE_UI_VULKAN_FUNCTION_PROMOTED

  // Non-promoted extensions, and extensions promoted to a Vulkan version not
  // supported by the device.
#define XE_UI_VULKAN_FUNCTION_PROMOTED(extension_name, core_name) \
  functions_loaded &= (dfn.core_name = PFN_##core_name(           \
                           ifn.vkGetDeviceProcAddr(device->device_, #extension_name))) != nullptr;
  if (properties.apiVersion < VK_MAKE_API_VERSION(0, 1, 1, 0)) {
    if (device->extensions_.ext_1_1_KHR_get_memory_requirements2) {
#include <rex/ui/vulkan/functions/device_1_1_khr_get_memory_requirements2.inc>
    }
    if (device->extensions_.ext_1_1_KHR_bind_memory2) {
#include <rex/ui/vulkan/functions/device_1_1_khr_bind_memory2.inc>
    }
  }
  if (properties.apiVersion < VK_MAKE_API_VERSION(0, 1, 3, 0)) {
    if (device->extensions_.ext_1_3_KHR_dynamic_rendering) {
#include <rex/ui/vulkan/functions/device_1_3_khr_dynamic_rendering.inc>
    }
    if (device->extensions_.ext_1_3_KHR_maintenance4) {
#include <rex/ui/vulkan/functions/device_1_3_khr_maintenance4.inc>
    }
  }
  if (device->extensions_.ext_KHR_swapchain) {
#include <rex/ui/vulkan/functions/device_khr_swapchain.inc>
  }
#undef XE_UI_VULKAN_FUNCTION_PROMOTED

#undef XE_UI_VULKAN_FUNCTION

  if (device->extensions_.ext_AMD_buffer_marker) {
    dfn.vkCmdWriteBufferMarkerAMD = reinterpret_cast<PFN_vkCmdWriteBufferMarkerAMD>(
        ifn.vkGetDeviceProcAddr(device->device_, "vkCmdWriteBufferMarkerAMD"));
    if (!dfn.vkCmdWriteBufferMarkerAMD) device->extensions_.ext_AMD_buffer_marker = false;
  }
  if (device->extensions_.ext_GOOGLE_display_timing) {
    dfn.vkGetRefreshCycleDurationGOOGLE = reinterpret_cast<PFN_vkGetRefreshCycleDurationGOOGLE>(
        ifn.vkGetDeviceProcAddr(device->device_, "vkGetRefreshCycleDurationGOOGLE"));
    dfn.vkGetPastPresentationTimingGOOGLE = reinterpret_cast<PFN_vkGetPastPresentationTimingGOOGLE>(
        ifn.vkGetDeviceProcAddr(device->device_, "vkGetPastPresentationTimingGOOGLE"));
    if (!dfn.vkGetRefreshCycleDurationGOOGLE || !dfn.vkGetPastPresentationTimingGOOGLE) {
      device->extensions_.ext_GOOGLE_display_timing = false;
      REXLOG_WARN("Presentation timing entry points unavailable; using software pacing");
    }
  }

  if (!functions_loaded) {
    REXLOG_ERROR("Failed to get all Vulkan device function pointers for '{}'",
                 properties.deviceName);
    return nullptr;
  }
  if (REXCVAR_GET(vulkan_command_counters)) InstallCommandCounters(dfn);

  // Get the queues.

  for (size_t queue_family_index = 0; queue_family_index < device->queue_families_.size();
       ++queue_family_index) {
    QueueFamily& queue_family = device->queue_families_[queue_family_index];
    for (size_t queue_index = 0; queue_index < queue_family.queues.size(); ++queue_index) {
      VkQueue queue;
      dfn.vkGetDeviceQueue(device->device_, uint32_t(queue_family_index), uint32_t(queue_index),
                           &queue);
      queue_family.queues[queue_index] = std::make_unique<Queue>(queue);
    }
  }

  // Get the memory types.

  VkPhysicalDeviceMemoryProperties memory_properties;
  ifn.vkGetPhysicalDeviceMemoryProperties(physical_device, &memory_properties);
  for (uint32_t memory_type_index = 0; memory_type_index < memory_properties.memoryTypeCount;
       ++memory_type_index) {
    const uint32_t memory_type_bit = uint32_t(1) << memory_type_index;
    const VkMemoryPropertyFlags memory_type_flags =
        memory_properties.memoryTypes[memory_type_index].propertyFlags;
    if (memory_type_flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) {
      device->memory_types_.device_local |= memory_type_bit;
    }
    if (memory_type_flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
      device->memory_types_.host_visible |= memory_type_bit;
    }
    if (memory_type_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) {
      device->memory_types_.host_coherent |= memory_type_bit;
    }
    if (memory_type_flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) {
      device->memory_types_.host_cached |= memory_type_bit;
    }
  }

  // The device works, so nothing was missing.
  g_device_requirement_failure.clear();
  return device;
}

VulkanDevice::~VulkanDevice() {
  if (device_) {
    vulkan_instance_->functions().vkDestroyDevice(device_, nullptr);
  }
}

VulkanDevice::VulkanDevice(const VulkanInstance* const vulkan_instance,
                           const VkPhysicalDevice physical_device)
    : vulkan_instance_(vulkan_instance), physical_device_(physical_device) {
  assert_not_null(vulkan_instance);
  assert_not_null(physical_device);
}

}  // namespace vulkan
}  // namespace ui
}  // namespace rex
