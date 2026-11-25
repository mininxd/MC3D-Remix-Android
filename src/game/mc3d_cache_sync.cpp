#include "game/mc3d_cache_sync.h"

#include <cstdint>
#include <mutex>

#include "game/d3d_guest_sync.h"
#include "game/guest_mem_safe.h"
#include "game/render_accuracy_internal.h"
#include "gpu/state.h"
#include "kernel/kernel.h"
#include "xdk/d3d8.h"

namespace game {
namespace mc3d_cache {

namespace {

// Verified from sub_002D25C0, sub_00032E30, sub_002D3A60, sub_000E2B90 in default.xbe:
// - 0x00558C58..0x00559187: 166 entries x 8 bytes { uint8_t valid; uint8_t pad[3]; uint32_t value; }
// - 0x005594B8..0x005596B7: uint32_t mc_tss_shadow[4][32] (0x80 bytes per stage)
constexpr uint32_t kMcRsCacheBase   = 0x00558C58u;
constexpr uint32_t kMcRsCacheCount  = 166u;
constexpr uint32_t kMcRsCacheStride = 8u;
constexpr uint32_t kMcTssShadowBase = 0x005594B8u;
constexpr uint32_t kMcTssStageBytes = 0x80u;

}  // namespace

void SyncAndInvalidateAll() {
    // 1. Force MC3D to miss texture, palette, PS, and VS shadow comparisons.
    for (uint32_t i = 0; i < 4u; ++i) {
        render_accuracy::SafeW32(render_accuracy::addr::kMcTexturesShadow + i * 4u, 0xFFFFFFFFu);
        render_accuracy::SafeW32(render_accuracy::addr::kMcPalettesShadow + i * 4u, 0xFFFFFFFFu);
    }
    render_accuracy::SafeW32(render_accuracy::addr::kMcPsShadow, 0xFFFFFFFFu);
    render_accuracy::SafeW32(render_accuracy::addr::kMcVsShadow, 0xFFFFFFFFu);

    // 2. Clear the valid byte on all 166 entries of MC3D's D3DRS shadow cache (matching sub_002D25C0).
    if (safe_mem::CanAccess(kMcRsCacheBase, kMcRsCacheCount * kMcRsCacheStride)) {
        uint8_t* p = static_cast<uint8_t*>(kernel::HostPtr(kMcRsCacheBase));
        for (uint32_t r = 0; r < kMcRsCacheCount; ++r) {
            p[r * kMcRsCacheStride] = 0u;
        }
    }

    // 3. Synchronize MC3D's 4x32 TSS shadow array directly to the live D3D device state so
    //    MC3D's TSS equality checks stay coherent after StateBlock apply/restore.
    if (safe_mem::CanAccess(kMcTssShadowBase, 4u * kMcTssStageBytes)) {
        std::lock_guard<std::recursive_mutex> lock(d3d::Lock());
        gpu::State& s = d3d::State();
        d3d_sync::PullGuestDeferredToHost(s);
        for (uint32_t st = 0; st < 4u; ++st) {
            const uint32_t stage_va = kMcTssShadowBase + st * kMcTssStageBytes;
            for (uint32_t slot = 0; slot < gpu::kTssCount; ++slot) {
                kernel::W32(stage_va + slot * 4u, s.tss[st][slot]);
            }
        }
    }
}

}  // namespace mc3d_cache
}  // namespace game
