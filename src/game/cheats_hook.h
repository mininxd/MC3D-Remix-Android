#pragma once

#include <cstdint>

struct X86Context;

namespace game::cheats {

void OnFrameTick();
void SyncPromptVisibility(X86Context& ctx, uint8_t* base);

}  // namespace game::cheats
