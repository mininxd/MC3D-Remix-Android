#include "game/render_accuracy.h"
#include "game/render_accuracy_internal.h"
#include "game/deferred_tss_guard.h"
#include "game/object_logger.h"

#include <cstdint>
#include <cstring>

#include "cpu/x86_context.h"
#include "cpu/x86_ops.h"
#include "xdk/hle.h"

// Helpers generating clean strong wrappers around generated X86_WEAK_FUNC symbols.
// State setters invalidate shadows without mutating TSS before textures are bound.
#define WRAP_POST(name)                                                       \
    extern "C" void __imp__##name(X86Context& ctx, uint8_t* base);            \
    extern "C" void name(X86Context& ctx, uint8_t* base) {                    \
        __imp__##name(ctx, base);                                             \
        game::render_accuracy::InvalidateMc3dStateShadows();                  \
    }

#define WRAP_PRE(name)                                                        \
    extern "C" void __imp__##name(X86Context& ctx, uint8_t* base);            \
    extern "C" void name(X86Context& ctx, uint8_t* base) {                    \
        game::render_accuracy::ScopedGuestTssGuard tss_guard;                 \
        game::render_accuracy::ApplyRenderAccuracy();                         \
        __imp__##name(ctx, base);                                             \
    }

#define WRAP_PUSH_DRAW(name)                                                  \
    extern "C" void __imp__##name(X86Context& ctx, uint8_t* base);            \
    extern "C" void name(X86Context& ctx, uint8_t* base) {                    \
        __imp__##name(ctx, base);                                             \
        game::render_accuracy::FlushCapturedPushSpan(ctx, base, 0, #name);    \
    }

// ============================================================================
// 1. Missing XDK 5849 D3D8 HLE Entry Points (left weak/unhooked by d3d8.cpp)
// ============================================================================

// D3DDevice_SetPalette(stage, palette) - stdcall (ret 0x08)
extern "C" void __imp__sub_00412580(X86Context& ctx, uint8_t* base);
extern "C" void sub_00412580(X86Context& ctx, uint8_t* base) {
    const uint32_t stage = game::render_accuracy::SafeR32(ctx.esp + 4u) & 3u;
    const uint32_t pal = game::render_accuracy::SafeR32(ctx.esp + 8u);
    __imp__sub_00412580(ctx, base);
    game::render_accuracy::NotifyPaletteBound(stage, pal);
}

// D3DDevice_BeginPush(dwords) - stdcall (ret 0x04) & inline wrapper sub_00416390
extern "C" void __imp__sub_00416860(X86Context& ctx, uint8_t* base);
extern "C" void sub_00416860(X86Context& ctx, uint8_t* base) {
    const uint32_t dwords = game::render_accuracy::SafeR32(ctx.esp + 4u);
    __imp__sub_00416860(ctx, base);
    game::render_accuracy::RecordPushSpanBegin(ctx.eax, dwords);
}

extern "C" void __imp__sub_00416390(X86Context& ctx, uint8_t* base);
extern "C" void sub_00416390(X86Context& ctx, uint8_t* base) {
    const uint32_t req = game::render_accuracy::SafeR32(ctx.esp + 4u) + 1u;
    const uint32_t saved_edx = ctx.edx;
    __imp__sub_00416390(ctx, base);
    (void)saved_edx;
    game::render_accuracy::RecordPushSpanBegin(ctx.eax, req);
}

// D3DDevice_ApplyStateBlock(token) & deferred restore helpers
extern "C" void __imp__sub_00416DFF(X86Context& ctx, uint8_t* base);
extern "C" void sub_00416DFF(X86Context& ctx, uint8_t* base) {
    __imp__sub_00416DFF(ctx, base);
    game::render_accuracy::InvalidateMc3dStateShadows();
}

WRAP_POST(sub_00418060)
WRAP_POST(sub_00417F40)
WRAP_POST(sub_00414830)
WRAP_POST(sub_00417B70)
WRAP_POST(sub_004150F0)

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
WRAP_POST(sub_002D17C0)
WRAP_DRAW_OBJ(sub_002D6830)
WRAP_DRAW_OBJ(sub_002D6A18)

// ============================================================================
// 4. All MC3D Pushbuffer EndPush Callers (10/10) + Immediate Mode Begin/End
// ============================================================================

WRAP_POST(sub_002D5D90)

extern "C" void __imp__sub_002D5E40(X86Context& ctx, uint8_t* base);
extern "C" void sub_002D5E40(X86Context& ctx, uint8_t* base) {
    __imp__sub_002D5E40(ctx, base);
    const uint32_t end = game::render_accuracy::SafeR32(game::render_accuracy::addr::kMcImmPushCur);
    game::render_accuracy::FlushCapturedPushSpan(ctx, base, end, "sub_002D5E40");
}

WRAP_PUSH_DRAW(sub_00041480)
WRAP_PUSH_DRAW(sub_00041BA0)
WRAP_PUSH_DRAW(sub_000E37D0)
WRAP_PUSH_DRAW(sub_00332810)
WRAP_PUSH_DRAW(sub_002D18E0)
WRAP_PUSH_DRAW(sub_002D1B50)
WRAP_PUSH_DRAW(sub_002D1D50)
WRAP_PUSH_DRAW(sub_002D1F00)
WRAP_PUSH_DRAW(sub_002D1FC6)

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

WRAP_POST(sub_0037F096)
WRAP_POST(sub_0037F975)
WRAP_POST(sub_0037FBF6)
WRAP_POST(sub_00032E30)
WRAP_POST(sub_0004EB10)
WRAP_POST(sub_0004EBDD)
WRAP_POST(sub_0004ED67)
extern "C" void __imp__sub_002D17E0(X86Context& ctx, uint8_t* base);
extern "C" void sub_002D17E0(X86Context& ctx, uint8_t* base) {
    __imp__sub_002D17E0(ctx, base);
    game::render_accuracy::OnSwapDiagnostics();
}

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
