// Safety guards for unhooked XACT / DSound voice-notification callbacks in Garage & track switches.

#include <atomic>
#include <cstdint>
#include <cstring>

#include "core/log.h"
#include "cpu/x86_context.h"
#include "game/guest_mem_safe.h"
#include "kernel/kernel.h"

namespace {

std::atomic<uint32_t> g_guard_logs{0};

bool IsValidTrackObj(uint32_t obj) {
    if (!game::safe_mem::CanAccess(obj, 0x28u)) return false;
    const uint32_t hdr = game::safe_mem::Read32(obj + 0x04u, 0);
    if (hdr && !game::safe_mem::CanAccess(hdr, 0x10u)) return false;
    return true;
}

}  // namespace

// sub_0040AD79(track_obj) - thiscall (ecx = cue/engine), stdcall ret 0x04
extern "C" void __imp__sub_0040AD79(X86Context& ctx, uint8_t* base);
extern "C" void sub_0040AD79(X86Context& ctx, uint8_t* base) {
    const uint32_t arg0 = game::safe_mem::Read32(ctx.esp + 4u, 0);
    const bool this_ok = game::safe_mem::CanAccess(ctx.ecx, 0x18u);
    const uint32_t sub10 = this_ok ? game::safe_mem::Read32(ctx.ecx + 0x10u, 0) : 0u;
    const bool sub10_ok = (!sub10 || game::safe_mem::CanAccess(sub10, 0x10u));
    if (!this_ok || !sub10_ok || !IsValidTrackObj(arg0)) {
        if (++g_guard_logs <= 24u) {
            LOGR("[DANGER] xact_guard: skipped unsafe sub_0040AD79(ecx=%08X, arg0=%08X)", ctx.ecx, arg0);
        }
        ctx.eax = 0;
        ctx.esp += 8u;
        return;
    }
    // Fix Garage SIGSEGV (0x1D0080000 in __imp__sub_0040AD79+692):
    // game.cpp stubs sub_003C2240/sub_003C22D4 (GetMixBins) and sub_0040F566 to return S_OK
    // without writing to [ebp-0x80] (dwMixBinCount). Zeroing [esp-0x100..esp) guarantees
    // dwMixBinCount == 0 before __imp__sub_0040AD79 executes sub esp, 0x80.
    if (ctx.esp > 0x100u && game::safe_mem::CanAccess(ctx.esp - 0x100u, 0x100u)) {
        std::memset(kernel::HostPtr(ctx.esp - 0x100u), 0, 0x100u);
    }
    static std::atomic<uint32_t> g_ad79_ok{0};
    if (++g_ad79_ok <= 4u) {
        LOGR("[OK] xact_guard: sanitized sub_0040AD79 stack frame (esp=%08X)", ctx.esp);
    }
    __imp__sub_0040AD79(ctx, base);
}

// sub_0040AEEF(track_obj) - thiscall (ecx = cue/engine), stdcall ret 0x04
extern "C" void __imp__sub_0040AEEF(X86Context& ctx, uint8_t* base);
extern "C" void sub_0040AEEF(X86Context& ctx, uint8_t* base) {
    const uint32_t arg0 = game::safe_mem::Read32(ctx.esp + 4u, 0);
    const bool this_ok = game::safe_mem::CanAccess(ctx.ecx, 0x18u);
    const uint32_t sub10 = this_ok ? game::safe_mem::Read32(ctx.ecx + 0x10u, 0) : 0u;
    const bool sub10_ok = (!sub10 || game::safe_mem::CanAccess(sub10, 0x10u));
    if (!this_ok || !sub10_ok || !IsValidTrackObj(arg0)) {
        if (++g_guard_logs <= 24u) {
            LOGR("[DANGER] xact_guard: skipped unsafe sub_0040AEEF(ecx=%08X, arg0=%08X)", ctx.ecx, arg0);
        }
        ctx.eax = 0;
        ctx.esp += 8u;
        return;
    }
    __imp__sub_0040AEEF(ctx, base);
}

// sub_00409216(a0, event_pkt) - thiscall/stdcall ret 0x08
extern "C" void __imp__sub_00409216(X86Context& ctx, uint8_t* base);
extern "C" void sub_00409216(X86Context& ctx, uint8_t* base) {
    const uint32_t pkt = game::safe_mem::Read32(ctx.esp + 8u, 0);
    if (!game::safe_mem::CanAccess(pkt, 0x18u)) {
        if (++g_guard_logs <= 24u) {
            LOGR("[DANGER] xact_guard: skipped unsafe sub_00409216(pkt=%08X)", pkt);
        }
        ctx.eax = 0;
        ctx.esp += 12u;
        return;
    }
    const uint32_t track = game::safe_mem::Read32(pkt + 0x10u, 0);
    const uint32_t cmd = game::safe_mem::Read32(pkt + 0x14u, 0);
    if (!IsValidTrackObj(track) || !game::safe_mem::CanAccess(cmd, 0x08u)) {
        if (++g_guard_logs <= 24u) {
            LOGR("[DANGER] xact_guard: skipped unsafe sub_00409216(track=%08X, cmd=%08X)", track, cmd);
        }
        ctx.eax = 0;
        ctx.esp += 12u;
        return;
    }
    __imp__sub_00409216(ctx, base);
}
