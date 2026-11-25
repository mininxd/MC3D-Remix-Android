#include "game/ui_aspect_rules.h"

#include <algorithm>

namespace game {
namespace ui_aspect_rules {

namespace {

thread_local int tl_flash_menu_depth = 0;

}  // namespace

ScopedFlashMenu::ScopedFlashMenu() { ++tl_flash_menu_depth; }
ScopedFlashMenu::~ScopedFlashMenu() { --tl_flash_menu_depth; }

float AdjustScaleX(float base_ui_sx, bool ultrawide_ui) {
    if (!ultrawide_ui || tl_flash_menu_depth <= 0) return base_ui_sx;
    // MC3D's 720x480 Flash menu stage (sub_000D3C20) already insets 4:3 menus (~0.75-0.89)
    // inside 16:9. Applying raw 16:9->20.5:9 squeeze (0.7805) double-narrows main menu UI.
    // Compensate cleanly so menus read comfortably without hard stretching.
    return std::clamp(base_ui_sx * 1.20f, base_ui_sx, 0.96f);
}

bool ShouldKeepFullScreenQuad(int x0, int y0, int x1, int y1, unsigned guest_w, unsigned guest_h) {
    if (guest_w == 0u || guest_h == 0u) return false;
    return x0 <= 2 && y0 <= 2 &&
           x1 >= static_cast<int>(guest_w) - 2 &&
           y1 >= static_cast<int>(guest_h) - 2;
}

}  // namespace ui_aspect_rules
}  // namespace game
