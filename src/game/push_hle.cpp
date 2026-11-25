#include "game/push_hle.h"

#include <atomic>
#include <cstdint>
#include <cstring>

#include "core/log.h"
#include "cpu/x86_context.h"
#include "cpu/x86_ops.h"
#include "game/guest_mem_safe.h"
#include "game/object_logger.h"
#include "game/render_accuracy.h"
#include "game/render_accuracy_internal.h"
#include "gpu/state.h"
#include "kernel/kernel.h"

// Original generated D3D8 entry points called for scratch push space and resource release.
extern "C" void __imp__sub_00416860(X86Context& ctx, uint8_t* base);  // D3DDevice_BeginPush(dwords)
extern "C" void sub_004144B0(X86Context& ctx, uint8_t* base);         // D3D_InternalRelease(res)

namespace game {
namespace push_hle {

namespace {

constexpr uint32_t kTssDwords = 128u;
constexpr uint32_t kTssBytes = kTssDwords * 4u;
constexpr uint32_t kInternalRefStep = 0x00080000u;
constexpr uint32_t kRefCountMask = 0x0078FFFFu;

thread_local const char* tl_caller_tag = nullptr;
thread_local TssDiffSlot* tl_caller_slot = nullptr;

std::atomic<uint64_t> g_begin_push_calls{0};
std::atomic<uint64_t> g_set_palette_calls{0};

void RestoreUnmodifiedDiff(const TssDiffSlot& slot) {
    if (!slot.captured) return;

    if (slot.has_tss && safe_mem::CanAccess(render_accuracy::addr::kD3dDeferredTss, kTssBytes)) {
        for (uint32_t i = 0; i < kTssDwords; ++i) {
            if (slot.orig_tss[i] == slot.post_tss[i]) continue;
            const uint32_t ea = render_accuracy::addr::kD3dDeferredTss + i * 4u;
            // Only revert slots that still carry our temporary draw repair.
            if (kernel::R32(ea) == slot.post_tss[i]) {
                kernel::W32(ea, slot.orig_tss[i]);
            }
        }
    }

    if (slot.has_ps_modes && slot.orig_ps_modes != slot.post_ps_modes) {
        const uint32_t rs_ea = render_accuracy::addr::kD3dRenderState + gpu::kRsPsTextureModes * 4u;
        if (safe_mem::CanAccess(rs_ea, 4u) && kernel::R32(rs_ea) == slot.post_ps_modes) {
            kernel::W32(rs_ea, slot.orig_ps_modes);
        }
    }
}

void RunPushAccuracyWithDiff(const char* tag, TssDiffSlot& slot) {
    if (slot.captured) {
        RestoreUnmodifiedDiff(slot);
        slot.captured = false;
    }

    if (safe_mem::CanAccess(render_accuracy::addr::kD3dDeferredTss, kTssBytes)) {
        std::memcpy(slot.orig_tss.data(), kernel::HostPtr(render_accuracy::addr::kD3dDeferredTss), kTssBytes);
        slot.has_tss = true;
    } else {
        slot.has_tss = false;
    }

    const uint32_t rs_ea = render_accuracy::addr::kD3dRenderState + gpu::kRsPsTextureModes * 4u;
    if (safe_mem::CanAccess(rs_ea, 4u)) {
        slot.orig_ps_modes = kernel::R32(rs_ea);
        slot.has_ps_modes = true;
    } else {
        slot.has_ps_modes = false;
    }

    objlog::SetDrawHint(tag, 0u);
    render_accuracy::ApplyRenderAccuracy();

    if (slot.has_tss && safe_mem::CanAccess(render_accuracy::addr::kD3dDeferredTss, kTssBytes)) {
        std::memcpy(slot.post_tss.data(), kernel::HostPtr(render_accuracy::addr::kD3dDeferredTss), kTssBytes);
    }
    if (slot.has_ps_modes && safe_mem::CanAccess(rs_ea, 4u)) {
        slot.post_ps_modes = kernel::R32(rs_ea);
    }
    slot.captured = true;
}

void CallInternalRelease(X86Context& ctx, uint8_t* base, uint32_t res_va) {
    const uint32_t saved_eax = ctx.eax;
    const uint32_t saved_ecx = ctx.ecx;
    const uint32_t saved_edx = ctx.edx;
    X86_PUSH32(res_va);
    X86_PUSH32(0u);
    sub_004144B0(ctx, base);  // stdcall ret 0x04
    ctx.eax = saved_eax;
    ctx.ecx = saved_ecx;
    ctx.edx = saved_edx;
}

}  // namespace

ScopedPushCallerGuard::ScopedPushCallerGuard(const char* tag)
    : prev_tag_(tl_caller_tag), prev_slot_(tl_caller_slot) {
    tl_caller_tag = tag;
    tl_caller_slot = &slot_;
}

ScopedPushCallerGuard::~ScopedPushCallerGuard() {
    RestoreUnmodifiedDiff(slot_);
    if (slot_.captured) {
        objlog::ClearDrawHint();
    }
    tl_caller_tag = prev_tag_;
    tl_caller_slot = prev_slot_;
}

}  // namespace push_hle
}  // namespace game

// D3DDevice_BeginPush inline wrapper (sub_00416390, stdcall ret 0x04):
// Original XDK code invoked sub_0041DD70 (LazySetStateVB), which pushed raw NV2A state
// methods into g_scratch_push, triggered MakeSpace warnings, and corrupted g_state.simple[]
// when d3d8.cpp::Walk() parsed them. Here we skip LazySetStateVB, run ApplyRenderAccuracy()
// for quad callers right before their single EndPush draw, and reserve (dwords + 1).
extern "C" void sub_00416390(X86Context& ctx, uint8_t* base) {
    if (game::push_hle::tl_caller_slot != nullptr && game::push_hle::tl_caller_tag != nullptr) {
        game::push_hle::RunPushAccuracyWithDiff(game::push_hle::tl_caller_tag,
                                                *game::push_hle::tl_caller_slot);
    }

    const uint64_t n = ++game::push_hle::g_begin_push_calls;
    if (n == 1u) {
        LOGR("[OK] push_hle: clean BeginPush (sub_00416390) active — skipping LazySetStateVB");
    }

    if (game::safe_mem::CanAccess(ctx.esp + 4u, 4u)) {
        kernel::W32(ctx.esp + 4u, kernel::R32(ctx.esp + 4u) + 1u);
    }
    __imp__sub_00416860(ctx, base);  // tail-call stdcall ret 0x04
}

// D3DDevice_SetPalette(stage, palette) - stdcall ret 0x08 (HAS_D3DDevice_SetPalette == 0).
// Maintains exact XDK 5849 CDevice->Palettes[stage] and internal refcount (+0x80000 / -0x80000)
// without pushing raw NV2A 0x41B20 palette methods or triggering MakeSpace from 004125EE.
extern "C" void sub_00412580(X86Context& ctx, uint8_t* base) {
    const uint32_t stage = game::safe_mem::Read32(ctx.esp + 4u, 0u) & 3u;
    const uint32_t pal = game::safe_mem::Read32(ctx.esp + 8u, 0u);
    const uint32_t p_dev = game::safe_mem::Read32(game::render_accuracy::addr::kD3dDevicePtr, 0u);

    uint32_t old_pal = 0u;
    if (p_dev && game::safe_mem::CanAccess(p_dev + game::render_accuracy::addr::kDevPalettes + stage * 4u, 4u)) {
        old_pal = kernel::R32(p_dev + game::render_accuracy::addr::kDevPalettes + stage * 4u);
    }

    if (pal != old_pal) {
        if (pal && game::safe_mem::CanAccess(pal, 8u)) {
            kernel::W32(pal, kernel::R32(pal) + game::push_hle::kInternalRefStep);
        }
        if (p_dev && game::safe_mem::CanAccess(p_dev + game::render_accuracy::addr::kDevPalettes + stage * 4u, 4u)) {
            kernel::W32(p_dev + game::render_accuracy::addr::kDevPalettes + stage * 4u, pal);
        }
        if (old_pal && game::safe_mem::CanAccess(old_pal, 8u)) {
            const uint32_t next_common = kernel::R32(old_pal) - game::push_hle::kInternalRefStep;
            kernel::W32(old_pal, next_common);
            if ((next_common & game::push_hle::kRefCountMask) == 0u) {
                game::push_hle::CallInternalRelease(ctx, base, old_pal);
            }
        }
    }

    game::render_accuracy::NotifyPaletteBound(stage, pal);

    const uint64_t n = ++game::push_hle::g_set_palette_calls;
    if (n == 1u) {
        LOGR("[OK] push_hle: clean HLE SetPalette (sub_00412580) active");
    }

    ctx.eax = 0u;
    ctx.esp += 12u;  // stdcall ret 0x08
}
