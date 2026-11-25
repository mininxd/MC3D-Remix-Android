#pragma once
// Tracks guest physical/data addresses rendered into via GPU render targets / depth targets
// so object_logs.txt classifies sampled GPU render targets as px=RT instead of false-positive
// TEX_SOLID_CONST from stale guest RAM.

#include <cstdint>

#include "gpu/state.h"

namespace game {
namespace rt_tracker {

void RecordTargetData(uint32_t data_va);
void RecordActiveTargets(const gpu::State& s);
bool IsKnownRenderTarget(uint32_t data_va);
uint32_t TrackedTargetCount();

}  // namespace rt_tracker
}  // namespace game
