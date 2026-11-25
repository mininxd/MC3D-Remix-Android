#include "game/render_accuracy.h"
#include "game/render_accuracy_internal.h"
#include "game/deferred_tss_guard.h"
#include "game/guest_mem_safe.h"
#include "game/object_logger.h"
#include "game/push_hle.h"
#include "game/rt_tracker.h"
#include "game/stateblock_hle.h"

#include <cstdint>
#include <cstring>
#include <mutex>

#include "cpu/x86_context.h"
#include "cpu/x86_ops.h"
#include "xdk/d3d8.h"
#include "xdk/hle.h"

// Helpers generating clean strong wrappers around generated X86_WEAK_FUNC symbols.
// State setters invalidate shadows without mutating TSS before textures are bound.
#define WRAP_POST(name)                                                       \
    extern "C" void __imp__##name(X86Context& ctx, uint8_t* base);            \
    extern "C" void name(X86Context& ctx, uint8_t* base) {                    \
        __imp__##name(ctx, base);                                             \
        game::render_accuracy::InvalidateMc3dStateShadows();                  \
    }

#define WRAP_PUSH_CALLER(name)                                                \
    extern "C" void __imp__##name(X86Context& ctx, uint8_t* base);            \
    extern "C" void name(X86Context& ctx, uint8_t* base) {                    \
        game::push_hle::ScopedPushCallerGuard push_guard(#name);              \
        __imp__##name(ctx, base);                                             \
    }

#define WRAP_RT_BIND(name)                                                    \
    extern "C" void __imp__##name(X86Context& ctx, uint8_t* base);            \
    extern "C" void name(X86Context& ctx, uint8_t* base) {                    \
        __imp__##name(ctx, base);                                             \
        std::lock_guard<std::recursive_mutex> lock(d3d::Lock());              \
        game::rt_tracker::RecordActiveTargets(d3d::State());                  \
    }

// ============================================================================
// 1. Missing XDK 5849 D3D8 HLE Entry Points
//    (SetPalette sub_00412580 & BeginPush sub_00416390 live in push_hle.cpp;
//     StateBlock APIs & GetVertexShader live in stateblock_hooks.cpp.)
// ============================================================================

WRAP_POST(sub_00418060)
WRAP_POST(sub_00417F40)
WRAP_POST(sub_00414830)

// Internal XDK deferred state emitters (kept covered alongside high-level hooks).
WRAP_POST(sub_0041DCF0)
WRAP_POST(sub_0041C8A0)
WRAP_POST(sub_0041CC20)
WRAP_POST(sub_00420280)
WRAP_POST(sub_0041CE10)
WRAP_POST(sub_0041C730)
WRAP_POST(sub_0041DD70)
WRAP_POST(sub_0041DF10)

// ============================================================================
// 2. MC3D Lifecycle (Present/Swap Diagnostics; sub_002D2850 is owned by game.cpp)
// ============================================================================

extern "C" void __imp__sub_002D2990(X86Context& ctx, uint8_t* base);
extern "C" void sub_002D2990(X86Context& ctx, uint8_t* base) {
    __imp__sub_002D2990(ctx, base);
    game::render_accuracy::OnSwapDiagnostics();
}

// ============================================================================
// 3. All MC3D Indexed & User-Pointer Draw Dispatchers
// ============================================================================

#define WRAP_DRAW_OBJ(name)                                                   \
    extern "C" void __imp__##name(X86Context& ctx, uint8_t* base);            \
    extern "C" void name(X86Context& ctx, uint8_t* base) {                    \
        game::render_accuracy::ScopedGuestTssGuard tss_guard;                 \
        game::objlog::SetDrawHint(#name, ctx.ecx);                            \
        game::render_accuracy::ApplyRenderAccuracy();                         \
        __imp__##name(ctx, base);                                             \
        game::objlog::ClearDrawHint();                                        \
    }

WRAP_DRAW_OBJ(sub_000E74A0)
WRAP_DRAW_OBJ(sub_000E74E0)
WRAP_POST(sub_000E3650)
WRAP_DRAW_OBJ(sub_00051410)
WRAP_DRAW_OBJ(sub_000988A0)
WRAP_DRAW_OBJ(sub_002D6830)
WRAP_DRAW_OBJ(sub_002D6A18)

// ============================================================================
// 4. All MC3D Pushbuffer EndPush Callers (10/10) + RenderTarget Wrappers
// ============================================================================

// Immediate-mode End: applies accuracy once right before d3d8.cpp's sub_004163B0 draws.
extern "C" void __imp__sub_002D5E40(X86Context& ctx, uint8_t* base);
extern "C" void sub_002D5E40(X86Context& ctx, uint8_t* base) {
    game::render_accuracy::ScopedGuestTssGuard tss_guard;
    game::objlog::SetDrawHint("sub_002D5E40", 0u);
    game::render_accuracy::ApplyRenderAccuracy();
    __imp__sub_002D5E40(ctx, base);
    game::objlog::ClearDrawHint();
}

WRAP_PUSH_CALLER(sub_00041480)
WRAP_PUSH_CALLER(sub_00041BA0)
WRAP_PUSH_CALLER(sub_000E37D0)
WRAP_PUSH_CALLER(sub_00332810)
WRAP_PUSH_CALLER(sub_002D18E0)
WRAP_PUSH_CALLER(sub_002D1B50)
WRAP_PUSH_CALLER(sub_002D1D50)
WRAP_PUSH_CALLER(sub_002D1F00)
WRAP_PUSH_CALLER(sub_002D1FC6)

// Track GPU render targets bound via MC3D's SetRenderTarget wrappers (even if only cleared).
WRAP_RT_BIND(sub_000E40A0)
WRAP_RT_BIND(sub_002D21D0)

// ============================================================================
// 5. MC3D Shader, Texture, Palette & Stateblock Managers
// ============================================================================

extern "C" void __imp__sub_002D1810(X86Context& ctx, uint8_t* base);
extern "C" void sub_002D1810(X86Context& ctx, uint8_t* base) {
    const uint32_t ps_h = game::render_accuracy::SafeR32(ctx.esp + 4u);
    __imp__sub_002D1810(ctx, base);
    game::render_accuracy::NotifyPixelShaderSet(ps_h);
    game::render_accuracy::InvalidateMc3dStateShadows();
}

// ID3DXEffect state assignment applier: forwards light touches during StateBlock recording.
extern "C" void __imp__sub_0037F096(X86Context& ctx, uint8_t* base);
extern "C" void sub_0037F096(X86Context& ctx, uint8_t* base) {
    const bool recording = game::stateblock::IsRecording();
    if (recording && game::safe_mem::CanAccess(ctx.ecx, 0x20u)) {
        const uint32_t st_type = game::safe_mem::Read32(ctx.ecx + 0x04u, 0u);
        if ((st_type >> 24) == 3u) {
            const uint32_t light_idx = game::safe_mem::Read32(ctx.ecx + 0x08u, 0xFFu);
            const uint32_t flags = game::safe_mem::Read32(ctx.ecx + 0x18u, 0u);
            const bool is_enable = ((st_type & 0x00FFFFFFu) == 0x0Du);
            const bool is_set = ((flags & 0x04u) != 0u);
            game::stateblock::RecordLightTouch(light_idx, is_set, is_enable);
        }
    }
    __imp__sub_0037F096(ctx, base);
    if (!recording) {
        game::render_accuracy::InvalidateMc3dStateShadows();
    }
}

WRAP_POST(sub_0037F975)
WRAP_POST(sub_0037FBF6)
WRAP_POST(sub_00032E30)
WRAP_POST(sub_0004EB10)
WRAP_POST(sub_0004EBDD)
WRAP_POST(sub_0004ED67)
WRAP_POST(sub_002D17E0)

// ============================================================================
// 6. XAPILIB File Size Compatibility Fix (avoids broken game.cpp ord 76)
// ============================================================================

// sub_00017A60 = GetFileSize(HANDLE hFile), stdcall (ret 0x04).
// Uses NtQueryInformationFile (via sub_00017A22 GetFileSizeEx) instead of
// IoQueryVolumeInformation (ordinal 76) whose args are swapped in game.cpp.
extern "C" void sub_00017A22(X86Context& ctx, uint8_t* base);
extern "C" void sub_00017A60(X86Context& ctx, uint8_t* base) {
    game::render_accuracy::EnsureKernelCompatHooks();
    const uint32_t hFile = game::render_accuracy::SafeR32(ctx.esp + 4u);
    const uint32_t saved_ebp = ctx.ebp;
    const uint32_t saved_ebx = ctx.ebx;
    const uint32_t saved_esi = ctx.esi;
    const uint32_t saved_edi = ctx.edi;

    ctx.esp -= 8u;
    const uint32_t out_size_va = ctx.esp;
    game::render_accuracy::SafeW32(out_size_va, 0u);
    game::render_accuracy::SafeW32(out_size_va + 4u, 0u);

    X86_PUSH32(out_size_va);
    X86_PUSH32(hFile);
    X86_PUSH32(0u);
    sub_00017A22(ctx, base);  // stdcall ret 0x08

    const uint32_t result = (ctx.eax != 0) ? game::render_accuracy::SafeR32(out_size_va) : 0xFFFFFFFFu;
    ctx.esp += 8u;
    ctx.eax = result;
    ctx.ebp = saved_ebp;
    ctx.ebx = saved_ebx;
    ctx.esi = saved_esi;
    ctx.edi = saved_edi;
    ctx.esp += 8u;  // ret 0x04
}
