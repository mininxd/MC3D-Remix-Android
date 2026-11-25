#pragma once
// Bidirectional sync helpers between host d3d::State() and XDK 5849 guest D3D memory.

#include <cstdint>
#include <memory>

#include "gpu/state.h"

namespace game {
namespace d3d_sync {

constexpr uint32_t kD3dSimpleEncodeTable = 0x00447A98u;  // [92] x 4 bytes
constexpr uint32_t kD3dStreamsTable      = 0x00425228u;  // [16] x 12 bytes (stride, offset, vb)
constexpr uint32_t kD3dFvfShaderStruct   = 0x00425400u;
constexpr uint32_t kDevVertexShaderPtr   = 0x0794u;
constexpr uint32_t kDevVertexShaderHnd   = 0x0798u;
constexpr uint32_t kDevTransforms        = 0x0C60u;  // [10] x 64 bytes
constexpr uint32_t kDevViewport          = 0x0EE0u;  // 24 bytes

// Returns NV2A simple method offset (0x0000..0x1FFC) for D3DRS index < 92, or 0 if none/dummy.
uint32_t SimpleMethodForRs(uint32_t rs);

// Converts XDK transform index (0..9) to D3D8 transform index (2,3,16..19,256..259).
uint32_t D3d8TransformType(uint32_t xdk_type);

// Pulls guest deferred TSS (0x4228E0) and deferred RS (92..135) into host gpu::State.
void PullGuestDeferredToHost(gpu::State& s);

// Mirrors host simple states (57..91, plus 0..56 when PS active), CDevice+0x784 (PS),
// and CDevice+0x798 (VS handle) into guest RAM so XDK getters and lazy helpers stay consistent.
void MirrorHostStateToGuest(const gpu::State& s);

// Applies a single D3D render state (0..165) to both host gpu::State and guest 0x422AE0.
void ApplyRenderState(gpu::State& s, uint32_t rs, uint32_t val);

// Reads a single D3D render state (0..165) from host gpu::State (simple or rs array).
uint32_t ReadRenderState(const gpu::State& s, uint32_t rs);

// Applies a single texture stage state to both host gpu::State and guest 0x4228E0.
void ApplyTextureStageState(gpu::State& s, uint32_t stage, uint32_t slot, uint32_t val);

// Loads pixel shader combiner registers from ps->def into s.simple and kRsPsTextureModes.
void LoadPixelShaderIntoState(gpu::State& s, const std::shared_ptr<gpu::PixelShader>& ps);

}  // namespace d3d_sync
}  // namespace game
