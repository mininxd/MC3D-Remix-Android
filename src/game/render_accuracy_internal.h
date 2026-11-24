#pragma once
#include <cstdint>

struct X86Context;

namespace game {
namespace render_accuracy {

// Guest D3D and Midnight Club 3 renderer globals (XDK 5849, default.xbe).
namespace addr {
constexpr uint32_t kD3dDirtyFlags      = 0x004228D8u;
constexpr uint32_t kD3dDeferredTss     = 0x004228E0u;  // [4][32] x 4 bytes (0x80 per stage)
constexpr uint32_t kD3dRenderState     = 0x00422AE0u;  // [166] x 4 bytes
constexpr uint32_t kD3dPsTexModesRs    = 0x00422D00u;  // D3D_RenderState[136]
constexpr uint32_t kD3dTexFactorRs     = 0x00422D30u;  // D3D_RenderState[148]
constexpr uint32_t kD3dDevicePtr       = 0x00422D78u;

// CDevice member offsets (XDK 5849).
constexpr uint32_t kDevPush            = 0x0000u;
constexpr uint32_t kDevPushThreshold   = 0x0004u;
constexpr uint32_t kDevFlags           = 0x0008u;
constexpr uint32_t kDevPixelShader     = 0x0784u;
constexpr uint32_t kDevPsTexModes      = 0x0790u;
constexpr uint32_t kDevVertexShader    = 0x0794u;
constexpr uint32_t kDevIndexBuffer     = 0x07A0u;
constexpr uint32_t kDevVtxDwords       = 0x0BC8u;
constexpr uint32_t kDevVtxOffset       = 0x0BCCu;
constexpr uint32_t kDevTextures        = 0x0F88u;  // [4] x 4 bytes
constexpr uint32_t kDevPalettes        = 0x0F98u;  // [4] x 4 bytes

// MC3D gfx state shadows (written before calling D3D; skipped on shadow hit).
constexpr uint32_t kMcPsShadow         = 0x00558C44u;
constexpr uint32_t kMcPalettesShadow   = 0x00559490u;  // [4]
constexpr uint32_t kMcVsShadow         = 0x005594A0u;
constexpr uint32_t kMcTexturesShadow   = 0x005594A4u;  // [4]
constexpr uint32_t kMcImmPushCur       = 0x00559A60u;
}  // namespace addr

// Safe guest memory predicates and accessors.
bool CanAccessGuest(uint32_t va, uint32_t bytes);
uint32_t SafeR32(uint32_t va, uint32_t fallback = 0);
uint16_t SafeR16(uint32_t va, uint16_t fallback = 0);
bool SafeW32(uint32_t va, uint32_t value);

// Tracks PS_TEXTUREMODES set explicitly via SetRenderState vs SetPixelShader.
void NotifyPsTextureModesSet(uint32_t modes);
void NotifyPixelShaderSet(uint32_t ps_handle);
void NotifyPaletteBound(uint32_t stage, uint32_t pal_va);
void InvalidateMc3dStateShadows();

// Pushbuffer capture & HLE draw execution helpers used by render_draw_hooks.cpp.
void RecordPushSpanBegin(uint32_t begin_va, uint32_t max_dwords);
void FlushCapturedPushSpan(X86Context& ctx, uint8_t* base, uint32_t explicit_end, const char* tag);
void ExecuteDrawIndexedVerticesUP(X86Context& ctx, uint8_t* base,
                                  uint32_t prim, uint32_t index_count,
                                  uint32_t indices_va, uint32_t verts_va, uint32_t stride);
void OnSwapDiagnostics();

}  // namespace render_accuracy
}  // namespace game
