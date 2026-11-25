#include "game/deferred_tss_guard.h"
#include "game/effect_probe.h"
#include "game/guest_mem_safe.h"
#include "game/object_logger.h"
#include "game/render_accuracy.h"

#include <cstdint>

#include "cpu/x86_context.h"

// Effect emitter scopes and immediate/particle draw wrappers moved from render_draw_hooks.cpp
// so neon underglow, car lights, and particle batches report CPU vertex diffuse & caller context.

#define WRAP_EFFECT_SCOPE(name, eff_label)                                    \
    extern "C" void __imp__##name(X86Context& ctx, uint8_t* base);            \
    extern "C" void name(X86Context& ctx, uint8_t* base) {                    \
        game::objlog::ScopedEffectContext eff_guard(eff_label);               \
        __imp__##name(ctx, base);                                             \
    }

// High-level MC3D effect emitters (all X86_WEAK_FUNC in generated/game):
WRAP_EFFECT_SCOPE(sub_000FA5A0, "NEON_GLOW")
WRAP_EFFECT_SCOPE(sub_00099BB0, "CAR_LIGHT_FX")
WRAP_EFFECT_SCOPE(sub_000992C0, "LIGHT_QUAD_FX")
WRAP_EFFECT_SCOPE(sub_00039500, "PARTICLE_FX")
WRAP_EFFECT_SCOPE(sub_00095F00, "PARTICLE_BILLBOARD")

// Immediate-mode pushbuffer draw end: inspect vertex diffuse values before Walk/End executes.
extern "C" void __imp__sub_002D5E40(X86Context& ctx, uint8_t* base);
extern "C" void sub_002D5E40(X86Context& ctx, uint8_t* base) {
    game::render_accuracy::ScopedGuestTssGuard tss_guard;
    game::objlog::ScopedVertexProbe vprobe;
    game::objlog::ProbeImmediatePushColors();
    game::objlog::SetDrawHint("sub_002D5E40", 0u);
    game::render_accuracy::ApplyRenderAccuracy();
    __imp__sub_002D5E40(ctx, base);
    game::objlog::ClearDrawHint();
}

// Billboard / particle quad batcher (FVF 0x144): inspect colorArr ([esp+0x10], count=[esp+0x14]).
extern "C" void __imp__sub_002D6830(X86Context& ctx, uint8_t* base);
extern "C" void sub_002D6830(X86Context& ctx, uint8_t* base) {
    game::render_accuracy::ScopedGuestTssGuard tss_guard;
    game::objlog::ScopedVertexProbe vprobe;
    const uint32_t color_arr_va = game::safe_mem::Read32(ctx.esp + 0x10u, 0u);
    const int32_t count = static_cast<int32_t>(game::safe_mem::Read32(ctx.esp + 0x14u, 0u));
    game::objlog::ProbeParticleColorArray(color_arr_va, count);
    game::objlog::SetDrawHint("sub_002D6830", 0u);
    game::render_accuracy::ApplyRenderAccuracy();
    __imp__sub_002D6830(ctx, base);
    game::objlog::ClearDrawHint();
}
