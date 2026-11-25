#include "game/car_lights_fix.h"

#include <atomic>
#include <cstdint>

#include "core/log.h"
#include "gpu/state.h"

namespace game {
namespace car_lights_fix {

namespace {

std::atomic<uint64_t> g_additive_fog_fixes{0};

}  // namespace

void Apply(gpu::State& s) {
    const bool blend_on = (s.simple[gpu::kMBlendEnable / 4u] & 1u) != 0u;
    const uint32_t dst_blend = s.simple[gpu::kMDestBlend / 4u];

    // On NV2A/D3D8 additive passes (DestBlend == D3DBLEND_ONE == 1), shaders.cpp lerps
    // fogcolor.rgb into outc before additive blending, adding (1 - fog.a) * fogcolor on
    // top of dark areas and washing car lights/coronas/billboards out toward solid grey/white.
    if (blend_on && dst_blend == 1u && (s.rs[gpu::kRsFogEnable] & 1u) != 0u) {
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
