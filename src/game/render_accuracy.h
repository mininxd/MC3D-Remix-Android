#pragma once

#include <cstdint>

namespace game {
namespace render_accuracy {

// Applies comprehensive rendering accuracy synchronization:
// 1. Synchronizes all 166 render states (kRsDeferredFirst..kRsMax - 1) from guest tables
// 2. Synchronizes all 32 texture stage states across all 4 stages (including kTssTexCoordIndex)
// 3. Scans hardware push buffer for pending texture offsets, formats, and combiner commands
// 4. Resolves active stage textures and synthesizes dynamic headers for push-buffer swapped textures
// 5. Auto-recovers PS_TEXTUREMODES so bound textures never sample to opaque black
// 6. Synthesizes programmable PixelShader combiners from hardware NV2A registers (0xAC0, 0x1E40, 0x260, 0xAA0)
// 7. Configures fixed-function fallback states for active textures without explicit combiner setup
// 8. Synchronizes TFACTOR and fog color into shader constants
void ApplyRenderAccuracy();

// Initializes the render accuracy subsystem
void InitRenderAccuracy();

}  // namespace render_accuracy
}  // namespace game
