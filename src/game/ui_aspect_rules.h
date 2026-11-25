#pragma once

namespace game {
namespace ui_aspect_rules {

// Scoped marker for Flash/menu draws (sub_000D3A9C) so menu screens are not
// double-narrowed inside an ultrawide viewport while keeping in-game HUD exact.
struct ScopedFlashMenu {
    ScopedFlashMenu();
    ~ScopedFlashMenu();
    ScopedFlashMenu(const ScopedFlashMenu&) = delete;
    ScopedFlashMenu& operator=(const ScopedFlashMenu&) = delete;
};

// Adjusts base 2D UI horizontal scale uniformly for active menu context, ensuring
// ortho, quad blits, and font glyphs within the same pass remain pixel-aligned.
float AdjustScaleX(float base_ui_sx, bool ultrawide_ui);

// Returns true if a full-screen 2D quad covers the virtual guest framebuffer and
// should remain edge-to-edge (menu/fade backdrop plates) rather than pillarboxed.
bool ShouldKeepFullScreenQuad(int x0, int y0, int x1, int y1, unsigned guest_w, unsigned guest_h);

}  // namespace ui_aspect_rules
}  // namespace game
