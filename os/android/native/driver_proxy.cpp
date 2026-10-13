// Source-built Vulkan dispatch adapter for the unchanged official game engine.
// It neither patches libmain.so nor hooks dlopen/dlsym process-wide.
#include <jni.h>
#include <android/log.h>
#include <adrenotools/driver.h>
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <limits.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#define EXPORT extern "C" __attribute__((visibility("default")))

EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance, const char*);
EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(const VkInstanceCreateInfo*, const VkAllocationCallbacks*, VkInstance*);
EXPORT VKAPI_ATTR void VKAPI_CALL vkDestroyInstance(VkInstance, const VkAllocationCallbacks*);

namespace {
constexpr const char* kTag = "LibertyDriver";
constexpr uint32_t kMaxPhysicalDevices = 16;
std::mutex g_init_mutex;
std::atomic<bool> g_ready{false};
std::atomic<uint32_t> g_game_instances{0};
bool g_attempted = false;
bool g_custom = false;
// Identity of physical device 0 under the stock driver, read before a custom
// one is loaded. Empty when it could not be read.
std::string g_system_identity;
void* g_loader = nullptr; // Deliberately retained until process exit.
PFN_vkGetInstanceProcAddr g_get = nullptr;
PFN_vkDestroyInstance g_destroy = nullptr;
std::string g_arguments;
std::string g_status = "{\"ok\":false,\"error\":\"Driver bridge not initialized\"}";

std::string Quote(const std::string& value) {
  std::string out = "\"";
  for (unsigned char c : value) {
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '"': out += "\\\""; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          constexpr char hex[] = "0123456789abcdef";
          out += "\\u00";
          out += hex[c >> 4];
          out += hex[c & 15];
        } else {
          out += static_cast<char>(c);
        }
    }
  }
  return out + "\"";
}

std::string JavaString(JNIEnv* env, jstring value) {
  if (!value) return {};
  const char* bytes = env->GetStringUTFChars(value, nullptr);
  if (!bytes) return {};
  std::string result(bytes);
  env->ReleaseStringUTFChars(value, bytes);
  return result;
}

std::string CanonicalDirectory(const std::string& value) {
  char resolved[PATH_MAX];
  struct stat st{};
  if (value.empty() || value.front() != '/' || !realpath(value.c_str(), resolved) ||
      stat(resolved, &st) != 0 || !S_ISDIR(st.st_mode)) return {};
  return std::string(resolved) + "/";
}

bool RegularFile(const std::string& path) {
  struct stat st{};
  return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

std::string LoaderPath() {
  Dl_info info{};
  if (g_get && dladdr(reinterpret_cast<void*>(g_get), &info) && info.dli_fname)
    return info.dli_fname;
  return {};
}

std::string Failure(const std::string& why) {
  __android_log_print(ANDROID_LOG_ERROR, kTag, "%s", why.c_str());
  return "{\"ok\":false,\"mode\":" + Quote(g_custom ? "turnip" : "system") +
         ",\"error\":" + Quote(why) + ",\"loader_path\":" + Quote(LoaderPath()) + "}";
}

template<typename Fn>
Fn Get(VkInstance instance, const char* name) {
  return reinterpret_cast<Fn>(g_get(instance, name));
}

// What makes one driver distinguishable from another.
//
// Not the GPU name alone - that is the same physical chip either way - and not
// apiVersion, which two builds of the same driver family often share.
// driverVersion, driverName and driverInfo are what actually move between a
// stock driver and one taken from somewhere else.
std::string DeviceIdentity(const VkPhysicalDeviceProperties& basic,
                           const VkPhysicalDeviceDriverProperties& driver) {
  return std::to_string(driver.driverID) + "|" + std::to_string(basic.driverVersion) + "|" +
         std::string(driver.driverName) + "|" + std::string(driver.driverInfo) + "|" +
         std::string(basic.deviceName);
}

// Reads the stock driver's identity, using its own handle and nothing global.
//
// This is what makes supporting a non-Turnip driver possible at all.
// libadrenotools is documented to return a valid pointer and then quietly fall
// back to the original driver when its hook does not take, so something has to
// prove the swap really happened. For Turnip the driver ID alone proves it -
// the stock Adreno driver can never report Mesa. A custom QUALCOMM driver
// reports exactly what the stock one reports, so the ID proves nothing, and
// the only honest evidence is that the identity CHANGED.
// The apiVersion to ask an instance for. The platform loader of Android 10 to
// 12 reports Vulkan 1.1 even when the driver behind it (Turnip on an Adreno
// 6xx) offers 1.3; a 1.1 loader accepts a higher apiVersion, and the device
// decides which 1.2 functionality exists. So 1.2 is requested from any 1.1+
// loader, which is what the game's own instance does as well.
uint32_t ProbeApiVersion(uint32_t loader_api) {
  return loader_api >= VK_API_VERSION_1_1 ? VK_API_VERSION_1_2 : VK_API_VERSION_1_0;
}

bool ProbeSystemIdentity(std::string& identity) {
  void* handle = dlopen("/system/lib64/libvulkan.so", RTLD_NOW | RTLD_LOCAL);
  if (!handle) return false;
  const auto get = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
      dlsym(handle, "vkGetInstanceProcAddr"));
  if (!get) { dlclose(handle); return false; }
  const auto create = reinterpret_cast<PFN_vkCreateInstance>(get(VK_NULL_HANDLE, "vkCreateInstance"));
  const auto enumerate_version = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
      get(VK_NULL_HANDLE, "vkEnumerateInstanceVersion"));
  if (!create) { dlclose(handle); return false; }
  uint32_t api = VK_API_VERSION_1_0;
  if (enumerate_version && enumerate_version(&api) != VK_SUCCESS) api = VK_API_VERSION_1_0;
  api = ProbeApiVersion(api);

  VkApplicationInfo app{};
  app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app.pApplicationName = "Liberty System Driver Probe";
  app.apiVersion = api;
  VkInstanceCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  info.pApplicationInfo = &app;
  VkInstance instance = VK_NULL_HANDLE;
  if (create(&info, nullptr, &instance) != VK_SUCCESS || instance == VK_NULL_HANDLE) {
    dlclose(handle);
    return false;
  }

  bool found = false;
  const auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(
      get(instance, "vkEnumeratePhysicalDevices"));
  const auto properties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
      get(instance, "vkGetPhysicalDeviceProperties"));
  const auto properties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
      get(instance, "vkGetPhysicalDeviceProperties2"));
  uint32_t count = 0;
  if (enumerate && properties && enumerate(instance, &count, nullptr) == VK_SUCCESS &&
      count > 0 && count <= kMaxPhysicalDevices) {
    std::vector<VkPhysicalDevice> devices(count);
    if (enumerate(instance, &count, devices.data()) == VK_SUCCESS && count > 0) {
      VkPhysicalDeviceProperties basic{};
      properties(devices[0], &basic);
      VkPhysicalDeviceDriverProperties driver{};
      driver.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
      if (api >= VK_API_VERSION_1_1 && basic.apiVersion >= VK_API_VERSION_1_2 && properties2) {
        VkPhysicalDeviceProperties2 extended{};
        extended.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        extended.pNext = &driver;
        properties2(devices[0], &extended);
      }
      identity = DeviceIdentity(basic, driver);
      found = true;
    }
  }
  const auto destroy = reinterpret_cast<PFN_vkDestroyInstance>(get(instance, "vkDestroyInstance"));
  if (destroy) destroy(instance, nullptr);
  dlclose(handle);
  return found;
}

// The capabilities the native renderer refuses to start without; the check
// that enforces them is in ui/vulkan/vulkan_device.cpp (with_native_shader_support).
// Reported per device so that a GPU lacking one - an unfamiliar vendor driver,
// say - produces a readable message on the launcher instead of a renderer that
// fails inside the game. Each value is 1 (present), 0 (absent) or -1 (could not
// be queried); only an explicit 0 is ever treated as a problem.
std::string CapabilityJson(VkInstance instance, VkPhysicalDevice device, uint32_t requested_api,
                           uint32_t device_api) {
  const auto features = Get<PFN_vkGetPhysicalDeviceFeatures>(instance, "vkGetPhysicalDeviceFeatures");
  const auto features2 = Get<PFN_vkGetPhysicalDeviceFeatures2>(instance, "vkGetPhysicalDeviceFeatures2");
  auto flag = [](int value) { return std::to_string(value); };
  int int64 = -1, clamp = -1, clip = -1, sample_rate = -1, blend = -1;
  int runtime_array = -1, partially_bound = -1, device_address = -1;
  if (features) {
    VkPhysicalDeviceFeatures base{};
    features(device, &base);
    int64 = base.shaderInt64 ? 1 : 0;
    clamp = base.depthClamp ? 1 : 0;
    clip = base.shaderClipDistance ? 1 : 0;
    sample_rate = base.sampleRateShading ? 1 : 0;
    blend = base.independentBlend ? 1 : 0;
  }
  if (features2 && requested_api >= VK_API_VERSION_1_2 && device_api >= VK_API_VERSION_1_2) {
    VkPhysicalDeviceVulkan12Features v12{};
    v12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    VkPhysicalDeviceFeatures2 extended{};
    extended.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    extended.pNext = &v12;
    features2(device, &extended);
    runtime_array = v12.runtimeDescriptorArray ? 1 : 0;
    partially_bound = v12.descriptorBindingPartiallyBound ? 1 : 0;
    device_address = v12.bufferDeviceAddress ? 1 : 0;
  }
  return "{\"shaderInt64\":" + flag(int64) + ",\"depthClamp\":" + flag(clamp) +
         ",\"shaderClipDistance\":" + flag(clip) + ",\"sampleRateShading\":" + flag(sample_rate) +
         ",\"independentBlend\":" + flag(blend) + ",\"runtimeDescriptorArray\":" + flag(runtime_array) +
         ",\"descriptorBindingPartiallyBound\":" + flag(partially_bound) +
         ",\"bufferDeviceAddress\":" + flag(device_address) + "}";
}

// Uses only functions from the selected loader, never this proxy's exports.
// A custom selection requires Vulkan 1.2 driver identity support. RP6/T30
// supports this; failing explicitly is safer than guessing from a GPU name.
bool VerifyInstance(VkInstance instance, uint32_t requested_api,
                    std::string& devices_json, std::string& error) {
  const auto enumerate = Get<PFN_vkEnumeratePhysicalDevices>(instance, "vkEnumeratePhysicalDevices");
  const auto properties = Get<PFN_vkGetPhysicalDeviceProperties>(instance, "vkGetPhysicalDeviceProperties");
  const auto properties2 = Get<PFN_vkGetPhysicalDeviceProperties2>(instance, "vkGetPhysicalDeviceProperties2");
  if (!enumerate || !properties) {
    error = "Selected loader is missing physical-device query functions";
    return false;
  }
  if (g_custom && (requested_api < VK_API_VERSION_1_1 || !properties2)) {
    error = "Custom driver validation requires a Vulkan 1.1 instance and the properties2 query";
    return false;
  }
  uint32_t count = 0;
  VkResult result = enumerate(instance, &count, nullptr);
  if (result != VK_SUCCESS || count == 0 || count > kMaxPhysicalDevices) {
    error = "Physical-device enumeration failed or returned an unsupported count (result=" +
            std::to_string(result) + ", count=" + std::to_string(count) + ")";
    return false;
  }
  std::vector<VkPhysicalDevice> devices(count);
  result = enumerate(instance, &count, devices.data());
  if (result != VK_SUCCESS || count == 0 || count > devices.size()) {
    error = "Physical-device list changed or could not be read";
    return false;
  }
  devices_json = "[";
  for (uint32_t i = 0; i < count; ++i) {
    VkPhysicalDeviceProperties basic{};
    properties(devices[i], &basic);
    VkPhysicalDeviceDriverProperties driver{};
    driver.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
    if (requested_api >= VK_API_VERSION_1_1 && basic.apiVersion >= VK_API_VERSION_1_2 && properties2) {
      VkPhysicalDeviceProperties2 extended{};
      extended.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
      extended.pNext = &driver;
      properties2(devices[i], &extended);
    }
    // Did the swap actually happen? Two ways to know, and the cheap one first:
    // a driver that is not Qualcomm's own cannot be the stock Adreno driver,
    // which settles every Turnip case without needing a baseline at all.
    //
    // Otherwise fall back to comparing identities. This used to demand Mesa
    // Turnip outright, which made a proprietary Qualcomm driver - the kind
    // extracted from another device - impossible to use however good it was,
    // because it reports the same driverID as the driver it replaces. An
    // identity that has not moved is the real evidence of libadrenotools
    // having fallen back, and that is what is reported now.
    // Every custom driver is checked the same way, with no shortcut on driver
    // ID. A shortcut was tried - trust anything not reporting as Qualcomm -
    // and it passed a silent fallback as success on the very first device it
    // met: this phone's stock driver reports Vulkan 1.1, so the 1.2 driver
    // properties are never filled in and driverID reads 0 rather than
    // QUALCOMM_PROPRIETARY. The old Turnip-only test happened to catch that
    // (0 is not Mesa either); an ID-based test that is trying to ACCEPT
    // drivers cannot. The identity comparison does not care, because a
    // fallback produces the stock identity whatever the ID says.
    if (g_custom) {
      const std::string identity = DeviceIdentity(basic, driver);
      if (g_system_identity.empty()) {
        error = "A custom driver was selected, but this device's own driver could not be read "
                "first, so there is no way to tell whether the custom one actually loaded";
        return false;
      }
      if (identity == g_system_identity) {
        error = "The custom driver did not take effect - the loaded driver is identical to this "
                "device's own (" + std::string(basic.deviceName) + ", driverVersion " +
                std::to_string(basic.driverVersion) + ")";
        return false;
      }
    }
    if (i) devices_json += ",";
    devices_json += "{\"name\":" + Quote(basic.deviceName) +
                    ",\"api_version\":" + std::to_string(basic.apiVersion) +
                    ",\"driver_version\":" + std::to_string(basic.driverVersion) +
                    ",\"vendor_id\":" + std::to_string(basic.vendorID) +
                    ",\"device_id\":" + std::to_string(basic.deviceID) +
                    ",\"driver_id\":" + std::to_string(driver.driverID) +
                    ",\"driver_name\":" + Quote(driver.driverName) +
                    ",\"driver_info\":" + Quote(driver.driverInfo) +
                    ",\"features\":" + CapabilityJson(instance, devices[i], requested_api, basic.apiVersion) + "}";
  }
  devices_json += "]";
  return true;
}

bool Probe(std::string& devices, std::string& error) {
  const auto create = Get<PFN_vkCreateInstance>(VK_NULL_HANDLE, "vkCreateInstance");
  const auto version = Get<PFN_vkEnumerateInstanceVersion>(VK_NULL_HANDLE, "vkEnumerateInstanceVersion");
  uint32_t api = VK_API_VERSION_1_0;
  if (!create || (version && version(&api) != VK_SUCCESS)) {
    error = "Selected Vulkan loader could not provide instance creation/version";
    return false;
  }
  if (g_custom && api < VK_API_VERSION_1_1) {
    error = "The Vulkan loader of this Android version is older than Vulkan 1.1";
    return false;
  }
  api = ProbeApiVersion(api);
  VkApplicationInfo app{};
  app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app.pApplicationName = "Liberty Driver Validation";
  app.apiVersion = api;
  VkInstanceCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  info.pApplicationInfo = &app;
  VkInstance instance = VK_NULL_HANDLE;
  const VkResult result = create(&info, nullptr, &instance);
  if (result != VK_SUCCESS || instance == VK_NULL_HANDLE) {
    error = "Selected Vulkan driver failed the startup probe (VkResult=" + std::to_string(result) + ")";
    return false;
  }
  const bool valid = VerifyInstance(instance, api, devices, error);
  g_destroy(instance, nullptr);
  return valid;
}
} // namespace

EXPORT jint JNICALL JNI_OnLoad(JavaVM*, void*) { return JNI_VERSION_1_6; }

EXPORT jstring JNICALL Java_com_libertyrecomp_DriverBridge_nativeInit(
    JNIEnv* env, jclass, jstring native_dir_value, jstring driver_dir_value,
    jstring filename_value, jboolean custom_value, jboolean fallback_value) {
  std::lock_guard<std::mutex> lock(g_init_mutex);
  const std::string native_dir_arg = JavaString(env, native_dir_value);
  const std::string driver_dir_arg = JavaString(env, driver_dir_value);
  const std::string filename = JavaString(env, filename_value);
  if (env->ExceptionCheck()) return nullptr;
  const bool custom = custom_value == JNI_TRUE;
  const bool fallback = fallback_value == JNI_TRUE;
  const std::string arguments = Quote(native_dir_arg) + Quote(driver_dir_arg) + Quote(filename) +
                                (custom ? "1" : "0") + (fallback ? "1" : "0");
  if (g_attempted) {
    const std::string response = arguments == g_arguments ? g_status :
        "{\"ok\":false,\"error\":\"Restart the application process before changing drivers\"}";
    return env->NewStringUTF(response.c_str());
  }
  g_attempted = true;
  g_arguments = arguments;
  g_custom = custom;
  const std::string native_dir = CanonicalDirectory(native_dir_arg);
  std::string error;
  if (native_dir.empty()) {
    error = "Native library directory is missing or not an absolute directory";
  } else {
    // Test the exact bare-name lookup that the unchanged engine will make.
    // Absolute preloading is insufficient if another libvulkan was inserted
    // earlier in this same application namespace. Do not guess in that case.
    void* selected = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL | RTLD_NOLOAD);
    const auto selected_get = selected ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        dlsym(selected, "vkGetInstanceProcAddr")) : nullptr;
    if (selected_get != &vkGetInstanceProcAddr) {
      error = "The application Vulkan lookup does not resolve to the driver proxy; restart before loading the game";
    }
    if (selected) dlclose(selected);
  }
  const bool proxy_ok = error.empty();
  if (error.empty() && custom) {
    const std::string driver_dir = CanonicalDirectory(driver_dir_arg);
    char file_path[PATH_MAX];
    if (driver_dir.empty() || driver_dir.rfind("/data/", 0) != 0) {
      error = "Custom Vulkan driver must be in app-private /data storage";
    } else if (filename.empty() || filename.find('/') != std::string::npos ||
               filename == "." || filename == "..") {
      error = "Custom Vulkan driver filename is invalid";
    } else if (!realpath((driver_dir + filename).c_str(), file_path) ||
               std::string(file_path).rfind(driver_dir, 0) != 0 || !RegularFile(file_path)) {
      error = "Custom Vulkan driver file is missing or escapes its private directory";
    } else if (!RegularFile(native_dir + "libhook_impl.so") || !RegularFile(native_dir + "libmain_hook.so")) {
      error = "Custom driver support libraries are missing; native libraries must be extracted";
    } else {
      // Read the stock driver BEFORE the hook is installed - afterwards there
      // is no longer an unhooked loader to ask. Not fatal on its own: a Turnip
      // driver is recognised by its driver ID and needs no baseline, so only a
      // same-vendor driver is refused for the lack of one.
      if (!ProbeSystemIdentity(g_system_identity)) {
        g_system_identity.clear();
        __android_log_print(ANDROID_LOG_WARN, kTag,
                            "could not read this device's own driver identity before loading the custom one");
      }
      g_loader = adrenotools_open_libvulkan(RTLD_NOW | RTLD_LOCAL, ADRENOTOOLS_DRIVER_CUSTOM,
          driver_dir.c_str(), native_dir.c_str(), driver_dir.c_str(), filename.c_str(), nullptr, nullptr);
      if (!g_loader) error = "libadrenotools could not initialize the selected custom driver";
    }
  } else if (error.empty()) {
    // An absolute system path cannot resolve back to this app's proxy by SONAME.
    g_loader = dlopen("/system/lib64/libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    if (!g_loader) {
      const char* detail = dlerror();
      error = std::string("System Vulkan loader could not open: ") + (detail ? detail : "unknown error");
    }
  }
  std::string devices;
  if (error.empty()) {
    g_get = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(g_loader, "vkGetInstanceProcAddr"));
    g_destroy = reinterpret_cast<PFN_vkDestroyInstance>(dlsym(g_loader, "vkDestroyInstance"));
    if (!g_get || !g_destroy || g_get == &vkGetInstanceProcAddr || g_destroy == &vkDestroyInstance) {
      error = "Vulkan loader entrypoints are missing or resolve recursively to the proxy";
    } else if (!Probe(devices, error)) {
      // Keep the exact failure for the status below.
    }
  }
  // A custom driver that cannot run this GPU (Turnip on an Adreno 8xx or a
  // non-Adreno GPU) falls back to the device's own driver when the caller
  // allows it; whether that driver is good enough is decided by the caller.
  std::string fallback_from;
  if (!error.empty() && custom && fallback && proxy_ok) {
    __android_log_print(ANDROID_LOG_WARN, kTag, "custom driver failed (%s); trying the system driver",
                        error.c_str());
    fallback_from = error;
    error.clear();
    devices.clear();
    g_custom = false;
    g_get = nullptr;
    g_destroy = nullptr;
    g_loader = dlopen("/system/lib64/libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    if (!g_loader) {
      const char* detail = dlerror();
      error = std::string("System Vulkan loader could not open: ") + (detail ? detail : "unknown error");
    } else {
      g_get = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(g_loader, "vkGetInstanceProcAddr"));
      g_destroy = reinterpret_cast<PFN_vkDestroyInstance>(dlsym(g_loader, "vkDestroyInstance"));
      if (!g_get || !g_destroy || g_get == &vkGetInstanceProcAddr || g_destroy == &vkDestroyInstance) {
        error = "Vulkan loader entrypoints are missing or resolve recursively to the proxy";
      } else {
        Probe(devices, error);
      }
    }
    if (!error.empty()) error = fallback_from + "; the system driver failed too: " + error;
  }
  if (!error.empty()) {
    g_status = Failure(error);
  } else {
    g_status = "{\"ok\":true,\"mode\":" + Quote(g_custom ? "turnip" : "system") +
        ",\"error\":\"\",\"fallback_from\":" + Quote(fallback_from) +
        ",\"loader_path\":" + Quote(LoaderPath()) +
        ",\"devices\":" + devices + "}";
    g_ready.store(true, std::memory_order_release);
    __android_log_print(ANDROID_LOG_INFO, kTag, "Driver initialized: %s", g_status.c_str());
  }
  return env->NewStringUTF(g_status.c_str());
}

EXPORT jstring JNICALL Java_com_libertyrecomp_DriverBridge_nativeStatus(JNIEnv* env, jclass) {
  std::lock_guard<std::mutex> lock(g_init_mutex);
  return env->NewStringUTF(g_status.c_str());
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(
    const VkInstanceCreateInfo* info, const VkAllocationCallbacks* allocator, VkInstance* instance) {
  if (!g_ready.load(std::memory_order_acquire)) return VK_ERROR_INITIALIZATION_FAILED;
  const auto create = Get<PFN_vkCreateInstance>(VK_NULL_HANDLE, "vkCreateInstance");
  const VkResult result = create(info, allocator, instance);
  if (result != VK_SUCCESS) return result;
  const uint32_t api = info && info->pApplicationInfo && info->pApplicationInfo->apiVersion ?
      info->pApplicationInfo->apiVersion : VK_API_VERSION_1_0;
  std::string devices, error;
  if (!VerifyInstance(*instance, api, devices, error)) {
    __android_log_print(ANDROID_LOG_ERROR, kTag, "Game Vulkan instance rejected: %s", error.c_str());
    g_destroy(*instance, allocator);
    *instance = VK_NULL_HANDLE;
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  const uint32_t count = g_game_instances.fetch_add(1, std::memory_order_relaxed) + 1;
  if (count <= 8) __android_log_print(ANDROID_LOG_INFO, kTag,
      "Game Vulkan instance %u verified: mode=%s loader=%s devices=%s", count,
      g_custom ? "turnip" : "system", LoaderPath().c_str(), devices.c_str());
  return VK_SUCCESS;
}

EXPORT VKAPI_ATTR void VKAPI_CALL vkDestroyInstance(VkInstance instance, const VkAllocationCallbacks* allocator) {
  if (g_ready.load(std::memory_order_acquire)) g_destroy(instance, allocator);
}

EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char* name) {
  if (!g_ready.load(std::memory_order_acquire) || !name) return nullptr;
  const auto real = g_get(instance, name);
  if (!real) return nullptr;
  if (std::strcmp(name, "vkGetInstanceProcAddr") == 0)
    return reinterpret_cast<PFN_vkVoidFunction>(&vkGetInstanceProcAddr);
  if (std::strcmp(name, "vkCreateInstance") == 0)
    return reinterpret_cast<PFN_vkVoidFunction>(&vkCreateInstance);
  if (std::strcmp(name, "vkDestroyInstance") == 0)
    return reinterpret_cast<PFN_vkVoidFunction>(&vkDestroyInstance);
  return real;
}

EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char* name) {
  if (!g_ready.load(std::memory_order_acquire)) return nullptr;
  const auto fn = reinterpret_cast<PFN_vkGetDeviceProcAddr>(dlsym(g_loader, "vkGetDeviceProcAddr"));
  return fn ? fn(device, name) : nullptr;
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceVersion(uint32_t* version) {
  if (!g_ready.load(std::memory_order_acquire)) return VK_ERROR_INITIALIZATION_FAILED;
  const auto fn = Get<PFN_vkEnumerateInstanceVersion>(VK_NULL_HANDLE, "vkEnumerateInstanceVersion");
  if (fn) return fn(version);
  if (version) *version = VK_API_VERSION_1_0;
  return VK_SUCCESS;
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceExtensionProperties(
    const char* layer, uint32_t* count, VkExtensionProperties* properties) {
  if (!g_ready.load(std::memory_order_acquire)) return VK_ERROR_INITIALIZATION_FAILED;
  const auto fn = Get<PFN_vkEnumerateInstanceExtensionProperties>(VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties");
  return fn ? fn(layer, count, properties) : VK_ERROR_INITIALIZATION_FAILED;
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceLayerProperties(uint32_t* count, VkLayerProperties* properties) {
  if (!g_ready.load(std::memory_order_acquire)) return VK_ERROR_INITIALIZATION_FAILED;
  const auto fn = Get<PFN_vkEnumerateInstanceLayerProperties>(VK_NULL_HANDLE, "vkEnumerateInstanceLayerProperties");
  return fn ? fn(count, properties) : VK_ERROR_INITIALIZATION_FAILED;
}
