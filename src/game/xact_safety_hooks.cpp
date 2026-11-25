// Safety guards for unhooked XACT / DSound voice-notification callbacks in Garage & track switches.

#include <cstdint>

#include "core/log.h"
#include "cpu/x86_context.h"
#include "game/guest_mem_safe.h"

namespace {

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
        LOG_FIRST(16, "xact_guard: skipped unsafe sub_0040AD79(ecx=%08X, arg0=%08X)", ctx.ecx, arg0);
        ctx.eax = 0;
        ctx.esp += 8u;
        return;
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
        LOG_FIRST(16, "xact_guard: skipped unsafe sub_0040AEEF(ecx=%08X, arg0=%08X)", ctx.ecx, arg0);
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
        LOG_FIRST(16, "xact_guard: skipped unsafe sub_00409216(pkt=%08X)", pkt);
        ctx.eax = 0;
        ctx.esp += 12u;
        return;
    }
    const uint32_t track = game::safe_mem::Read32(pkt + 0x10u, 0);
    const uint32_t cmd = game::safe_mem::Read32(pkt + 0x14u, 0);
    if (!IsValidTrackObj(track) || !game::safe_mem::CanAccess(cmd, 0x08u)) {
        LOG_FIRST(16, "xact_guard: skipped unsafe sub_00409216(track=%08X, cmd=%08X)", track, cmd);
        ctx.eax = 0;
        ctx.esp += 12u;
        return;
    }
    __imp__sub_00409216(ctx, base);
}
