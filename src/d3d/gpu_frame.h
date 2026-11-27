#pragma once
#include <cstdint>

namespace gpu {
// until the GPU has drawn present number n, counted from 0; any thread
void WaitGpuFrame(uint64_t n);

// Dirty-range bitmask between d3d8.cpp state setters and render_thread.cpp SendState()
// so unchanged State blocks (vsc 3KB, lights/material 900B, transforms, etc.) skip per-draw NEON scans.
enum StateDirtyBits : uint32_t {
    kDirtySimple       = 1u << 0,  // simple render states & pixel shader combiner constants (ranges 0, 1, 2)
    kDirtyRsTss        = 1u << 1,  // rs[] & tss[][] (range 3)
    kDirtyIndex        = 1u << 2,  // index_buffer / index_data / base_vertex / vs_handle (range 4)
    kDirtyVsc          = 1u << 3,  // vsc[192] vertex shader constants (range 5, 3072 B)
    kDirtyScissor      = 1u << 4,  // scissor_count / scissors / viewport / zscale / ss_offset (range 6)
    kDirtyTransformVP  = 1u << 5,  // transforms[2..3] view & proj (range 7)
    kDirtyTransformTex = 1u << 6,  // transforms[16..19] texture matrices (range 8)
    kDirtyTransformW   = 1u << 7,  // transforms[256] world matrix (range 9)
    kDirtyLighting     = 1u << 8,  // material & 8 lights (range 10, ~900 B)
    kDirtyAll          = 0x1FFu,
};
void MarkStateDirty(uint32_t bits);
uint32_t ConsumeStateDirty();
}  // namespace gpu
