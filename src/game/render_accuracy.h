#pragma once
#include <cstdint>

namespace game {
namespace render_accuracy {

// Initializes render-accuracy instrumentation and pushbuffer/D3D state tracking.
void InitRenderAccuracy();

// Synchronizes and repairs XDK/MC3D state (textures, palettes, pixel-shader
// texture modes, and fixed-function combiners) immediately before draws.
void ApplyRenderAccuracy();

}  // namespace render_accuracy
}  // namespace game
