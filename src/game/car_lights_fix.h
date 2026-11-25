#pragma once

namespace gpu {
struct State;
}

namespace game {
namespace car_lights_fix {

// Fixes additive light/corona/particle fog washout and white solid bloom issues
// after guest state synchronization without corrupting MC3D's guest RS shadow cache.
void Apply(gpu::State& s);

}  // namespace car_lights_fix
}  // namespace game
