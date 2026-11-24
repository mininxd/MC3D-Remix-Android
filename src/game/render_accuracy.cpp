#include "game/render_accuracy.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>

#include "app/app.h"
#include "core/log.h"
#include "cpu/x86_ops.h"
#include "gpu/state.h"
#include "gpu/texture.h"
#include "image_symbols.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "xdk/d3d8.h"
#include "xdk/hle.h"
#include "xdk/layout.h"
#include "x86_recomp_shared.h"

namespace d3d {
gpu::State& State();
std::recursive_mutex& Lock();
}  // namespace d3d

using namespace kernel;

namespace game {
namespace render_accuracy {

namespace {

// Cached synthetic texture headers for stages 0..3 when textures are bound via push buffer
static uint32_t s_synth_tex_headers[4] = {0, 0, 0, 0};

// Cached synthetic programmable pixel shader for hardware combiner reproduction
static std::shared_ptr<gpu::PixelShader> s_synth_pixel_shader = nullptr;

// Track push buffer scanning progress
static uint32_t s_last_scanned_push = 0;

// Scans pending NV2A push buffer commands up to the current push buffer write pointer
void ScanPushBuffer(gpu::State& s) {
    uint32_t pDev = R32(SYM(D3D_pDevice));
    if (!pDev) return;
    uint32_t push_end = R32(pDev + XDK_CDevice_Push);
    if (!push_end) return;

    // Reset scan pointer if push buffer rewound (e.g. ScratchPush rewind)
    if (s_last_scanned_push == 0 || push_end < s_last_scanned_push ||
        (push_end - s_last_scanned_push) > 0x100000u) {
        s_last_scanned_push = (push_end > 0x4000u) ? (push_end - 0x4000u) : push_end;
    }

    for (uint32_t p = s_last_scanned_push; p + 4 <= push_end;) {
        uint32_t head = R32(p);
        uint32_t method = head & 0x1FFCu;
        uint32_t n = (head >> 18) & 0x7FFu;
        p += 4;
        if ((head & 0xA0030003u) || n * 4 > push_end - p) {
            // Not a method header (jump, call, or invalid token)
            continue;
        }
        for (uint32_t i = 0; i < n && p + 4 <= push_end; i++, p += 4) {
            uint32_t m = (head & 0x40000000u) ? method : (method + i * 4);
            uint32_t v = R32(p);
            if ((m >> 2) < s.simple.size()) {
                s.simple[m >> 2] = v;
            }
        }
    }
    s_last_scanned_push = push_end;
}

// Synchronizes active stage textures from guest CDevice table and hardware registers
void SyncStageTextures(gpu::State& s) {
    uint32_t pDev = R32(SYM(D3D_pDevice));
    for (int st = 0; st < 4; st++) {
        // 1. Check if guest CDevice table has a bound texture header
        if (pDev) {
            uint32_t guest_tex = R32(pDev + XDK_CDevice_Textures + st * 4);
            if (guest_tex != 0) {
                s.textures[st] = guest_tex;
            }
        }

        // 2. Check hardware push buffer texture offset (0x1B00 + st * 0x40) and format (0x1B04 + st * 0x40)
        uint32_t pb_offset = s.simple[(0x1B00u + st * 0x40u) >> 2];
        uint32_t pb_format = s.simple[(0x1B04u + st * 0x40u) >> 2];
        uint32_t pb_pitch = s.simple[(0x1B10u + st * 0x40u) >> 2];

        if (pb_offset != 0 && pb_format != 0) {
            if (!s_synth_tex_headers[st]) {
                s_synth_tex_headers[st] = mem::AllocSystem(64);
            }
            uint32_t h = s_synth_tex_headers[st];
            bool cube = (pb_format & 4u) != 0;
            W32(h + 0, cube ? 0x00070001u : 0x00040001u);  // Common: texture type, refcount 1
            W32(h + 4, pb_offset);                          // Data: physical address
            W32(h + 8, 0);                                  // Lock: 0
            W32(h + 12, pb_format);                         // Format: container format
            W32(h + 16, pb_pitch);                          // Size / Pitch
            W32(h + 20, 0);

            // If stage texture is currently unbound or points to invalid memory, bind synthetic header
            if (s.textures[st] == 0) {
                s.textures[st] = h;
            }
        }

        // 3. Texture addressing (wrap / clamp / mirror) from 0x1B08 + st * 0x40
        uint32_t pb_addr = s.simple[(0x1B08u + st * 0x40u) >> 2];
        if (pb_addr != 0 && s.tss[st][gpu::kTssAddressU] == 0) {
            s.tss[st][gpu::kTssAddressU] = pb_addr & 0xFu;
            s.tss[st][gpu::kTssAddressV] = (pb_addr >> 4) & 0xFu;
            s.tss[st][gpu::kTssAddressW] = (pb_addr >> 8) & 0xFu;
        }
    }
}

// Repairs PS_TEXTUREMODES so bound textures never sample to opaque black
void SyncTextureModes(gpu::State& s) {
    // Read hardware push buffer texture modes
    uint32_t pb_modes = s.simple[0x1E70u >> 2];
    if (pb_modes != 0) {
        s.rs[gpu::kRsPsTextureModes] = pb_modes;
    }

    uint32_t modes = s.rs[gpu::kRsPsTextureModes];
    for (int st = 0; st < 4; st++) {
        uint32_t m = (modes >> (st * 5)) & 0x1Fu;
        if (s.textures[st] != 0 && m == 0) {
            // Stage has a bound texture, but mode 0 forces shader to sample float4(0,0,0,1)
            // Auto-detect mode from texture descriptor
            gpu::TextureDesc td;
            uint32_t auto_mode = 1;  // standard 2D lookup
            if (gpu::Describe(s.textures[st], td)) {
                if (td.cube) auto_mode = 3;
                else if (td.depth > 1) auto_mode = 2;
                else auto_mode = 1;
            }
            modes |= (auto_mode << (st * 5));
        }
    }
    s.rs[gpu::kRsPsTextureModes] = modes;
    s.simple[0x1E70u >> 2] = modes;
}

// Synthesizes programmable PixelShader combiners from NV2A hardware registers
void SyncCombiners(gpu::State& s) {
    uint32_t combiner_count = s.simple[0x1E60u >> 2];

    if (combiner_count != 0 && s.ps == nullptr) {
        if (!s_synth_pixel_shader) {
            s_synth_pixel_shader = std::make_shared<gpu::PixelShader>();
        }
        uint32_t* def = s_synth_pixel_shader->def;

        // Alpha inputs (0..7): method 0x260
        for (int i = 0; i < 8; i++) def[i] = s.simple[(0x260u >> 2) + i];

        // Final combiner ABCD & EFGH: methods 0x288, 0x28C
        def[8] = s.simple[0x288u >> 2];
        def[9] = s.simple[0x28Cu >> 2];

        // Alpha outputs (26..33): method 0xAA0
        for (int i = 0; i < 8; i++) def[26 + i] = s.simple[(0xAA0u >> 2) + i];

        // RGB inputs (34..41): method 0xAC0
        for (int i = 0; i < 8; i++) def[34 + i] = s.simple[(0xAC0u >> 2) + i];

        // Compare mode: method 0x17F8
        def[42] = s.simple[0x17F8u >> 2];

        // RGB outputs (45..52): method 0x1E40
        for (int i = 0; i < 8; i++) def[45 + i] = s.simple[(0x1E40u >> 2) + i];

        // Combiner count: method 0x1E60
        def[53] = combiner_count;

        // Dot mapping: method 0x1E74
        def[55] = s.simple[0x1E74u >> 2];

        // Input texture: method 0x1E78
        def[56] = s.simple[0x1E78u >> 2];

        s_synth_pixel_shader->handle = 0x80000001u;
        s.ps = s_synth_pixel_shader;
    }

    // Fixed-function fallback: if still no pixel shader, ensure stage 0 modulates texture with diffuse
    if (s.ps == nullptr && s.textures[0] != 0 && s.tss[0][gpu::kTssColorOp] <= 1) {
        s.tss[0][gpu::kTssColorOp] = 4;  // D3DTOP_MODULATE
        s.tss[0][gpu::kTssColorArg1] = 2;  // D3DTA_TEXTURE
        s.tss[0][gpu::kTssColorArg2] = 1;  // D3DTA_CURRENT
        s.tss[0][gpu::kTssAlphaOp] = 2;   // D3DTOP_SELECTARG1
        s.tss[0][gpu::kTssAlphaArg1] = 2;  // D3DTA_TEXTURE
    }
}

}  // namespace

void ApplyRenderAccuracy() {
    std::lock_guard<std::recursive_mutex> lock(d3d::Lock());
    gpu::State& s = d3d::State();

    // 1. Scan any pending push buffer commands into s.simple
    ScanPushBuffer(s);

    // 2. Synchronize all deferred and complex render states (92..165)
    for (uint32_t i = gpu::kRsDeferredFirst; i < gpu::kRsMax; i++) {
        s.rs[i] = R32(SYM(D3D_RenderState) + i * 4);
    }

    // 3. Synchronize all 32 texture stage states across all 4 stages (including kTssTexCoordIndex)
    for (int st = 0; st < 4; st++) {
        uint32_t stage_base = SYM(D3D_DeferredTextureState) + st * 32 * 4;
        for (uint32_t i = 0; i < gpu::kTssCount; i++) {
            s.tss[st][i] = R32(stage_base + i * 4);
        }
    }

    // 4. Synchronize textures and synthesize headers if push buffer set
    SyncStageTextures(s);

    // 5. Repair texture modes so sampled textures are never opaque black
    SyncTextureModes(s);

    // 6. Synthesize programmable combiners from hardware NV2A registers
    SyncCombiners(s);

    // 7. Synchronize TextureFactor for TFACTOR combiner stages
    uint32_t tfactor = s.rs[gpu::kRsTextureFactor];
    if (tfactor == 0) {
        tfactor = R32(SYM(D3D_RenderState) + gpu::kRsTextureFactor * 4);
        if (tfactor != 0) s.rs[gpu::kRsTextureFactor] = tfactor;
    }
}

void InitRenderAccuracy() {
    LOGI("render_accuracy: initialized graphics accuracy subsystem");
}

}  // namespace render_accuracy
}  // namespace game

// =============================================================================
// Recompiler Weak-Symbol Overrides (Strong Definitions)
// These intercept critical graphics pipeline stages to ensure full accuracy.
// =============================================================================

// Inlined D3D dirty-flag state committer
X86_EXTERN_FUNC(__imp__sub_0041DCF0);
X86_FUNC(sub_0041DCF0) {
    __imp__sub_0041DCF0(ctx, base);
    game::render_accuracy::ApplyRenderAccuracy();
}

// Inlined push texture modes (PS_TEXTUREMODES)
X86_EXTERN_FUNC(__imp__sub_0041C8A0);
X86_FUNC(sub_0041C8A0) {
    __imp__sub_0041C8A0(ctx, base);
    game::render_accuracy::ApplyRenderAccuracy();
}

// Inlined push deferred texture states
X86_EXTERN_FUNC(__imp__sub_0041CC20);
X86_FUNC(sub_0041CC20) {
    __imp__sub_0041CC20(ctx, base);
    game::render_accuracy::ApplyRenderAccuracy();
}

// Inlined combiner setup (pushes 0x1E60 and 36 combiner dwords)
X86_EXTERN_FUNC(__imp__sub_00420280);
X86_FUNC(sub_00420280) {
    __imp__sub_00420280(ctx, base);
    game::render_accuracy::ApplyRenderAccuracy();
}

// Inlined push render states
X86_EXTERN_FUNC(__imp__sub_0041CE10);
X86_FUNC(sub_0041CE10) {
    __imp__sub_0041CE10(ctx, base);
    game::render_accuracy::ApplyRenderAccuracy();
}

// Inlined push viewport / scissor
X86_EXTERN_FUNC(__imp__sub_0041C730);
X86_FUNC(sub_0041C730) {
    __imp__sub_0041C730(ctx, base);
    game::render_accuracy::ApplyRenderAccuracy();
}

// Inlined DrawIndexedVertices dispatch
X86_EXTERN_FUNC(__imp__sub_0041DD70);
X86_FUNC(sub_0041DD70) {
    game::render_accuracy::ApplyRenderAccuracy();
    __imp__sub_0041DD70(ctx, base);
}

// Inlined DrawVerticesUP dispatch
X86_EXTERN_FUNC(__imp__sub_0041DF10);
X86_FUNC(sub_0041DF10) {
    game::render_accuracy::ApplyRenderAccuracy();
    __imp__sub_0041DF10(ctx, base);
}
