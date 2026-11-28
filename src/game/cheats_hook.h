#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "common/menu.h"

struct X86Context;

namespace game::cheats {

void OnFrameTick();
void SyncPromptVisibility(X86Context& ctx, uint8_t* base);

bool IsCollectionRow(int row_id);
const app::rows::Row* CollectionRows(size_t* count);
std::string FormatCollectionRowValue(int row_id, int step);

}  // namespace game::cheats
