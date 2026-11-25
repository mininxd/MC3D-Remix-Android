#include "game/vk_format_probe.h"

#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#if defined(__ANDROID__) || (defined(__has_include) && __has_include(<vulkan/vulkan.h>))
#include <vulkan/vulkan.h>
#define MC3DR_HAS_VULKAN_HEADER 1
#else
#define MC3DR_HAS_VULKAN_HEADER 0
#endif

namespace game {
namespace objlog {

namespace {

std::once_flag g_probe_once;
VkVertexFormatReport g_report{};

void RunProbeOnce() {
    g_report.probed = true;
#if MC3DR_HAS_VULKAN_HEADER
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "mc3dr_probe";
    app.apiVersion = VK_API_VERSION_1_0;

    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;

    VkInstance inst = VK_NULL_HANDLE;
    if (vkCreateInstance(&ici, nullptr, &inst) != VK_SUCCESS || inst == VK_NULL_HANDLE) {
        return;
    }

    uint32_t count = 0;
    if (vkEnumeratePhysicalDevices(inst, &count, nullptr) == VK_SUCCESS && count > 0) {
        std::vector<VkPhysicalDevice> gpus(count);
        if (vkEnumeratePhysicalDevices(inst, &count, gpus.data()) == VK_SUCCESS && count > 0) {
            VkPhysicalDevice chosen = gpus[0];
            VkPhysicalDeviceProperties props{};
            for (VkPhysicalDevice d : gpus) {
                vkGetPhysicalDeviceProperties(d, &props);
                if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ||
                    props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
                    chosen = d;
                    break;
                }
            }
            vkGetPhysicalDeviceProperties(chosen, &props);

            std::snprintf(g_report.device_name, sizeof(g_report.device_name), "%s", props.deviceName);
            g_report.api_version = props.apiVersion;
            g_report.driver_version = props.driverVersion;
            g_report.vendor_id = props.vendorID;
            g_report.device_id = props.deviceID;

            auto check_vb = [&](VkFormat fmt, uint32_t* out_bits) -> bool {
                VkFormatProperties fp{};
                vkGetPhysicalDeviceFormatProperties(chosen, fmt, &fp);
                if (out_bits) *out_bits = static_cast<uint32_t>(fp.bufferFeatures);
                return (fp.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) != 0;
            };

            g_report.bgra8_vb = check_vb(VK_FORMAT_B8G8R8A8_UNORM, &g_report.bgra8_buf_features);
            g_report.rgba8_vb = check_vb(VK_FORMAT_R8G8B8A8_UNORM, nullptr);
            g_report.snorm16x4_vb = check_vb(VK_FORMAT_R16G16B16A16_SNORM, nullptr);
            g_report.r32u_vb = check_vb(VK_FORMAT_R32_UINT, nullptr);
            g_report.vulkan_ok = true;
        }
    }

    vkDestroyInstance(inst, nullptr);
#endif
}

}  // namespace

const VkVertexFormatReport& ProbeVertexFormats() {
    std::call_once(g_probe_once, RunProbeOnce);
    return g_report;
}

bool IsBgra8VertexSupported() {
    const VkVertexFormatReport& r = ProbeVertexFormats();
    return !r.vulkan_ok || r.bgra8_vb;
}

std::string FormatVkProbeLine() {
    const VkVertexFormatReport& r = ProbeVertexFormats();
    char buf[320];
    if (!r.vulkan_ok) {
        std::snprintf(buf, sizeof(buf),
                      "[VK_PROBE] status=UNAVAILABLE(stub_or_no_inst) assume_bgra8_vb=YES");
        return buf;
    }
    std::snprintf(buf, sizeof(buf),
                  "[VK_PROBE] gpu=\"%s\" vendor=0x%04X dev=0x%04X api=%u.%u.%u drv=0x%08X "
                  "BGRA8_VB=%s(feat=0x%X) RGBA8_VB=%s SNORM16x4_VB=%s R32U_VB=%s",
                  r.device_name, r.vendor_id, r.device_id,
                  (r.api_version >> 22) & 0x7Fu, (r.api_version >> 12) & 0x3FFu, r.api_version & 0xFFFu,
                  r.driver_version,
                  r.bgra8_vb ? "YES" : "NO", r.bgra8_buf_features,
                  r.rgba8_vb ? "YES" : "NO",
                  r.snorm16x4_vb ? "YES" : "NO",
                  r.r32u_vb ? "YES" : "NO");
    return buf;
}

}  // namespace objlog
}  // namespace game
