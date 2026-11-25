#include "game/p8_palette_probe.h"

#include <cstring>

#include "game/guest_mem_safe.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

namespace game {
namespace objlog {

bool ReadXboxPalette(uint32_t pal_va, std::array<uint32_t, 256>& out_bgra) {
    out_bgra.fill(0u);
    if (!pal_va || !safe_mem::CanAccess(pal_va, 8u)) return false;

    // Matches native/vulkan.cpp ReadStagePalette:
    // +0: Common (bits 30..31 = size_code: 0=256, 1=128, 2=64, 3=32 entries)
    // +4: Data   (physical address of BGRA8 palette table)
    const uint32_t common = safe_mem::Read32(pal_va + 0u, 0u);
    const uint32_t phys = safe_mem::Read32(pal_va + 4u, 0u);
    if (!phys) return false;

    const uint32_t data_va = mem::VirtualFromPhysical(phys);
    const uint32_t size_code = (common >> 30u) & 3u;
    const uint32_t entries = 256u >> size_code;
    const uint32_t bytes = entries * sizeof(uint32_t);
    if (!safe_mem::CanAccess(data_va, bytes)) return false;

    std::memcpy(out_bgra.data(), kernel::HostPtr(data_va), bytes);
    return true;
}

bool IsCompressedBlockTransparent(uint32_t format, uint32_t w0, uint32_t w1) {
    // DXT3 (0x0E): bytes 0..7 are 16 explicit 4-bit alpha values.
    if (format == 0x0Eu) {
        return w0 == 0u && w1 == 0u;
    }
    // DXT5 (0x0F): bytes 0..1 are a0/a1, bytes 2..7 are 16 x 3-bit alpha indices.
    // All-zero alpha half means a0==0, a1==0, and every index==0 -> alpha==0 everywhere.
    if (format == 0x0Fu) {
        return w0 == 0u && w1 == 0u;
    }
    return false;
}

bool IsUncompressedDwordTransparent(uint32_t format, uint32_t w0) {
    switch (format) {
        case 0x06u: case 0x12u:  // A8R8G8B8, LIN_A8R8G8B8
        case 0x3Au: case 0x3Fu:  // A8B8G8R8, LIN_A8B8G8R8
            return (w0 & 0xFF000000u) == 0u;
        case 0x3Bu: case 0x40u:  // B8G8R8A8, LIN_B8G8R8A8
        case 0x3Cu: case 0x41u:  // R8G8B8A8, LIN_R8G8B8A8
            return (w0 & 0x000000FFu) == 0u;
        case 0x04u: case 0x1Du:  // A4R4G4B4, LIN_A4R4G4B4
            return (w0 & 0xF000F000u) == 0u;
        case 0x02u: case 0x10u:  // A1R5G5B5, LIN_A1R5G5B5
            return (w0 & 0x80008000u) == 0u;
        case 0x38u: case 0x3Du:  // R5G5B5A1, LIN_R5G5B5A1
            return (w0 & 0x00010001u) == 0u;
        case 0x39u: case 0x3Eu:  // R4G4B4A4, LIN_R4G4B4A4
            return (w0 & 0x000F000Fu) == 0u;
        case 0x19u: case 0x1Fu:  // A8, LIN_A8
        case 0x01u: case 0x1Bu:  // AL8, LIN_AL8
            return w0 == 0u;
        case 0x1Au: case 0x20u:  // A8L8, LIN_A8L8
            return (w0 & 0xFF00FF00u) == 0u;
        default:
            return false;
    }
}

}  // namespace objlog
}  // namespace game
