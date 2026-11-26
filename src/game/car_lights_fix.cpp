#include "game/car_lights_fix.h"

#include <atomic>
#include <cstdint>
#include <cstring>

#include "core/log.h"
#include "game/d3d_guest_sync.h"
#include "game/effect_probe.h"
#include "gpu/state.h"

namespace game {
namespace car_lights_fix {

namespace {

constexpr uint32_t kRsAlphaBlendEnable = 0x3Du;
constexpr uint32_t kRsBlendOp          = 0x4Au;
constexpr uint32_t kBlendOne           = 1u;
constexpr uint32_t kBlendSrcAlpha      = 0x302u;
constexpr uint32_t kBlendOpAdd         = 0x8006u;
constexpr uint32_t kBlendOpRevSubtract = 0x800Bu;
constexpr uint32_t kTopDisable         = 1u;
constexpr uint32_t kTopModulate        = 4u;
constexpr uint32_t kTaDiffuse          = 0u;
constexpr uint32_t kTaTexture          = 2u;

std::atomic<uint64_t> g_additive_fog_fixes{0};
std::atomic<uint64_t> g_neon_state_fixes{0};
std::atomic<uint64_t> g_additive_blendop_fixes{0};

}  // namespace

void Apply(gpu::State& s) {
    const char* eff = game::objlog::ActiveEffectName();
    const bool is_neon = (eff != nullptr && std::strcmp(eff, "NEON_GLOW") == 0);

    // sub_000FA5A0(0x10) draws car neon underglow tubes (fx_car_neon_glow) and the ground glow pool
    // (vp_shadow.tex) via fixed-function vglBegin/vglEnd immediately after sub_00106E30 (car ground
    // shadow). sub_00106E30 leaves D3DRS_BLENDOP=REVSUBTRACT (0x800B) and/or a shadow pixel shader /
    // TSS active, while sub_002D3660(2, 0x80) only sets SRCBLEND=SRCALPHA and DESTBLEND=ONE.
    // Restore clean additive fixed-function modulate state so neon glows with its true RGB color.
    if (is_neon) {
        const uint32_t prev_op = s.simple[gpu::kMBlendOp / 4u];
        const bool had_ps = (s.ps != nullptr);
        if (had_ps) {
            d3d_sync::LoadPixelShaderIntoState(s, nullptr);
        }
        d3d_sync::ApplyRenderState(s, kRsAlphaBlendEnable, 1u);
        d3d_sync::ApplyRenderState(s, kRsBlendOp, kBlendOpAdd);
        d3d_sync::ApplyTextureStageState(s, 0u, gpu::kTssColorOp, kTopModulate);
        d3d_sync::ApplyTextureStageState(s, 0u, gpu::kTssColorArg1, kTaTexture);
        d3d_sync::ApplyTextureStageState(s, 0u, gpu::kTssColorArg2, kTaDiffuse);
        d3d_sync::ApplyTextureStageState(s, 0u, gpu::kTssAlphaOp, kTopModulate);
        d3d_sync::ApplyTextureStageState(s, 0u, gpu::kTssAlphaArg1, kTaTexture);
        d3d_sync::ApplyTextureStageState(s, 0u, gpu::kTssAlphaArg2, kTaDiffuse);
        d3d_sync::ApplyTextureStageState(s, 1u, gpu::kTssColorOp, kTopDisable);
        d3d_sync::ApplyTextureStageState(s, 1u, gpu::kTssAlphaOp, kTopDisable);
        s.rs[gpu::kRsFogEnable] = 0u;

        const uint64_t n = ++g_neon_state_fixes;
        if (n <= 4u) {
            LOGR("[OK] car_lights_fix: restored additive FF state for NEON_GLOW (prevOp=0x%X hadPs=%u count=%llu)",
                 prev_op, had_ps ? 1u : 0u, static_cast<unsigned long long>(n));
        }
    }

    const bool blend_on = (s.simple[gpu::kMBlendEnable / 4u] & 1u) != 0u;
    const uint32_t src_blend = s.simple[gpu::kMSrcBlend / 4u];
    const uint32_t dst_blend = s.simple[gpu::kMDestBlend / 4u];
    const uint32_t blend_op = s.simple[gpu::kMBlendOp / 4u];

    // Any SRCALPHA -> ONE additive pass inheriting stale REVSUBTRACT (0x800B) from a preceding
    // car shadow draw (which uses ONE -> ONE REVSUBTRACT) must use ADD.
    if (blend_on && src_blend == kBlendSrcAlpha && dst_blend == kBlendOne && blend_op == kBlendOpRevSubtract) {
        d3d_sync::ApplyRenderState(s, kRsBlendOp, kBlendOpAdd);
        const uint64_t n = ++g_additive_blendop_fixes;
        if (n <= 4u) {
            LOGR("[OK] car_lights_fix: reset stale REVSUBTRACT to ADD on SRCALPHA->ONE pass (count=%llu)",
                 static_cast<unsigned long long>(n));
        }
    }

    // On NV2A/D3D8 additive passes (DestBlend == D3DBLEND_ONE == 1), shaders.cpp lerps
    // fogcolor.rgb into outc before additive blending, adding (1 - fog.a) * fogcolor on
    // top of dark areas and washing car lights/coronas/billboards out toward solid grey/white.
    if (blend_on && dst_blend == kBlendOne && (s.rs[gpu::kRsFogEnable] & 1u) != 0u) {
        s.rs[gpu::kRsFogEnable] = 0u;
        const uint64_t n = ++g_additive_fog_fixes;
        if (n <= 4u) {
            LOGI("car_lights_fix: cleared FOGENABLE on additive pass (dstBlend=ONE, count=%llu)",
                 static_cast<unsigned long long>(n));
        }
    }
}

}  // namespace car_lights_fix
}  // namespace game
