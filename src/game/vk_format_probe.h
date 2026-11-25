#pragma once
// Probes Vulkan physical device vertex-attribute format support at startup
// (notably VK_FORMAT_B8G8R8A8_UNORM VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT on mobile GPUs).

#include <cstdint>
#include <string>

namespace game {
namespace objlog {

struct VkVertexFormatReport {
    bool probed = false;
    bool vulkan_ok = false;
    bool bgra8_vb = true;       // VK_FORMAT_B8G8R8A8_UNORM (D3DCOLOR 0x40)
    bool rgba8_vb = true;       // VK_FORMAT_R8G8B8A8_UNORM
    bool snorm16x4_vb = true;   // VK_FORMAT_R16G16B16A16_SNORM
    bool r32u_vb = true;        // VK_FORMAT_R32_UINT
    uint32_t bgra8_buf_features = 0;
    uint32_t api_version = 0;
    uint32_t driver_version = 0;
    uint32_t vendor_id = 0;
    uint32_t device_id = 0;
    char device_name[128] = "unknown";
};

// Executes the one-time hardware probe (thread-safe) and returns cached results.
const VkVertexFormatReport& ProbeVertexFormats();

// Convenience check: returns true unless Vulkan proved B8G8R8A8_UNORM lacks VERTEX_BUFFER_BIT.
bool IsBgra8VertexSupported();

// Formats a single-line human-readable summary of the Vulkan vertex format probe.
std::string FormatVkProbeLine();

}  // namespace objlog
}  // namespace game
