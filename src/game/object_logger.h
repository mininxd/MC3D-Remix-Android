#pragma once
// Public exports for 3D model, texture load, and per-object draw accuracy logging (object_logs.txt).

#include <cstdint>

#include "gpu/state.h"

namespace game {
namespace objlog {

void Init();
void SetDrawHint(const char* tag, uint32_t obj_va);
void ClearDrawHint();

void LogModelLoad(const char* kind, const char* name, uint32_t obj_va, uint32_t aux);
void LogResourcePageIn(const char* kind, uint32_t rsc_va, uint32_t arr_va, uint32_t count);
void LogDrawState(const gpu::State& s);
void OnSwapSummary();

}  // namespace objlog
}  // namespace game
