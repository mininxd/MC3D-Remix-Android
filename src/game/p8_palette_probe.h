#pragma once
// Xbox D3DPalette reader and alpha-transparency helpers for accurate P8/DXT/RGBA
// texel inspection without false-positive black/white errors.

#include <array>
#include <cstdint>

namespace game {
namespace objlog {

// Reads an Xbox D3DPalette header's BGRA8 colour table (matching native/vulkan.cpp).
// Returns false if the palette header or its backing memory is not accessible.
bool ReadXboxPalette(uint32_t pal_va, std::array<uint32_t, 256>& out_bgra);

// Returns true when a sampled compressed block's alpha channel is provably 100% transparent (0).
bool IsCompressedBlockTransparent(uint32_t format, uint32_t w0, uint32_t w1);

// Returns true when an uncompressed pixel format carries an alpha channel and every pixel
// packed into sample_dword has alpha == 0.
bool IsUncompressedDwordTransparent(uint32_t format, uint32_t sample_dword);

}  // namespace objlog
}  // namespace game
