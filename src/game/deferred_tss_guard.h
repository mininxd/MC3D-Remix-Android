#pragma once
// RAII guard preserving guest deferred D3D TSS (0x4228E0) and PS texture mode RAM across a draw.

#include <cstdint>
#include <cstring>

#include "game/guest_mem_safe.h"
#include "game/render_accuracy_internal.h"
#include "gpu/state.h"
#include "kernel/kernel.h"

namespace game {
namespace render_accuracy {

class ScopedGuestTssGuard {
public:
    ScopedGuestTssGuard() {
        if (safe_mem::CanAccess(addr::kD3dDeferredTss, kTssBytes)) {
            std::memcpy(saved_tss_, kernel::HostPtr(addr::kD3dDeferredTss), kTssBytes);
            has_tss_ = true;
        }
        const uint32_t rs_ea = addr::kD3dRenderState + gpu::kRsPsTextureModes * 4u;
        if (safe_mem::CanAccess(rs_ea, 4u)) {
            saved_ps_modes_ = kernel::R32(rs_ea);
            has_ps_modes_ = true;
        }
        const uint32_t lit_ea = addr::kD3dRenderState + gpu::kRsLighting * 4u;
        if (safe_mem::CanAccess(lit_ea, 4u)) {
            saved_lighting_ = kernel::R32(lit_ea);
            has_lighting_ = true;
        }
        const uint32_t fog_ea = addr::kD3dRenderState + gpu::kRsFogEnable * 4u;
        if (safe_mem::CanAccess(fog_ea, 4u)) {
            saved_fog_enable_ = kernel::R32(fog_ea);
            has_fog_enable_ = true;
        }
    }

    ~ScopedGuestTssGuard() {
        if (has_tss_ && safe_mem::CanAccess(addr::kD3dDeferredTss, kTssBytes)) {
            std::memcpy(kernel::HostPtr(addr::kD3dDeferredTss), saved_tss_, kTssBytes);
        }
        if (has_ps_modes_) {
            const uint32_t rs_ea = addr::kD3dRenderState + gpu::kRsPsTextureModes * 4u;
            if (safe_mem::CanAccess(rs_ea, 4u)) kernel::W32(rs_ea, saved_ps_modes_);
        }
        if (has_lighting_) {
            const uint32_t lit_ea = addr::kD3dRenderState + gpu::kRsLighting * 4u;
            if (safe_mem::CanAccess(lit_ea, 4u)) kernel::W32(lit_ea, saved_lighting_);
        }
        if (has_fog_enable_) {
            const uint32_t fog_ea = addr::kD3dRenderState + gpu::kRsFogEnable * 4u;
            if (safe_mem::CanAccess(fog_ea, 4u)) kernel::W32(fog_ea, saved_fog_enable_);
        }
    }

    ScopedGuestTssGuard(const ScopedGuestTssGuard&) = delete;
    ScopedGuestTssGuard& operator=(const ScopedGuestTssGuard&) = delete;

private:
    static constexpr uint32_t kTssBytes = 4u * 0x80u;
    uint8_t saved_tss_[kTssBytes]{};
    uint32_t saved_ps_modes_ = 0;
    uint32_t saved_lighting_ = 0;
    uint32_t saved_fog_enable_ = 0;
    bool has_tss_ = false;
    bool has_ps_modes_ = false;
    bool has_lighting_ = false;
    bool has_fog_enable_ = false;
};

}  // namespace render_accuracy
}  // namespace game
