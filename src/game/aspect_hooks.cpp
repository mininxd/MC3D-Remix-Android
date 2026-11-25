#include "game/aspect_fix.h"
#include "game/ui_aspect_rules.h"

#include <cstdint>

#include "cpu/x86_context.h"

// Thin strong wrappers overriding generated X86_WEAK_FUNC symbols for:
//  - True ultrawide HOR+ 3D cameras (pixel aspect ratio sync)
//  - Un-stretched 2D GUI / HUD / Flash / minimap rendering
//  - Native pass-through Bink FMV playback (bypassing 2D HUD pillarbox mapping)
//  - Fullscreen fade and post-FX preservation

// 1. Display mode setup -> sync MC3D pixel aspect ratio [0x004B4E88] to host Aspect().
extern "C" void __imp__sub_002D2290(X86Context& ctx, uint8_t* base);
extern "C" void sub_002D2290(X86Context& ctx, uint8_t* base) {
    __imp__sub_002D2290(ctx, base);
    game::aspect_fix::SyncPixelAspect();
}

// 2. gfxViewport::SetWindow -> keep [0x004B4E88] synced + map minimap sub-viewport X/W.
extern "C" void __imp__sub_002D4F10(X86Context& ctx, uint8_t* base);
extern "C" void sub_002D4F10(X86Context& ctx, uint8_t* base) {
    game::aspect_fix::AdjustSetWindowArgs(ctx);
    __imp__sub_002D4F10(ctx, base);
}

// 3. 2D GUI / Flash / HUD Ortho viewport initializer (all 6 callers are pure 2D UI).
extern "C" void __imp__sub_000D3690(X86Context& ctx, uint8_t* base);
extern "C" void sub_000D3690(X86Context& ctx, uint8_t* base) {
    game::aspect_fix::ScopedUiOrtho guard;
    __imp__sub_000D3690(ctx, base);
}

// 3b. Flash menu renderer entry (Main Menu / Pause) -> prevent double-narrowing on ultrawide.
extern "C" void __imp__sub_000D3A9C(X86Context& ctx, uint8_t* base);
extern "C" void sub_000D3A9C(X86Context& ctx, uint8_t* base) {
    game::ui_aspect_rules::ScopedFlashMenu guard;
    __imp__sub_000D3A9C(ctx, base);
}

// 4. gfxViewport::Ortho -> pillarbox full-screen 2D UI ortho bounds when inside sub_000D3690.
extern "C" void __imp__sub_002D4D30(X86Context& ctx, uint8_t* base);
extern "C" void sub_002D4D30(X86Context& ctx, uint8_t* base) {
    game::aspect_fix::AdjustOrthoArgs(ctx);
    __imp__sub_002D4D30(ctx, base);
}

// 5. Bink FMV renderer (intro, story, tutorial, reward videos) -> native pass-through.
extern "C" void __imp__sub_0015AF30(X86Context& ctx, uint8_t* base);
extern "C" void sub_0015AF30(X86Context& ctx, uint8_t* base) {
    game::aspect_fix::ScopedFullScreenBypass guard;
    __imp__sub_0015AF30(ctx, base);
}

extern "C" void __imp__sub_00363950(X86Context& ctx, uint8_t* base);
extern "C" void sub_00363950(X86Context& ctx, uint8_t* base) {
    game::aspect_fix::ScopedFullScreenBypass guard;
    __imp__sub_00363950(ctx, base);
}

// 6. Minimap / radar screen sub-viewports -> align sub-viewport rect with un-stretched HUD.
extern "C" void __imp__sub_000B9210(X86Context& ctx, uint8_t* base);
extern "C" void sub_000B9210(X86Context& ctx, uint8_t* base) {
    game::aspect_fix::ScopedMinimapSubViewport guard;
    __imp__sub_000B9210(ctx, base);
}

extern "C" void __imp__sub_000BA210(X86Context& ctx, uint8_t* base);
extern "C" void sub_000BA210(X86Context& ctx, uint8_t* base) {
    game::aspect_fix::ScopedMinimapSubViewport guard;
    __imp__sub_000BA210(ctx, base);
}

// 7. HUD sprite helpers that divide width by [0x004B4E88] -> prevent double horizontal squish.
extern "C" void __imp__sub_000D1CB0(X86Context& ctx, uint8_t* base);
extern "C" void sub_000D1CB0(X86Context& ctx, uint8_t* base) {
    game::aspect_fix::ScopedRefPixelAspect guard;
    __imp__sub_000D1CB0(ctx, base);
}

extern "C" void __imp__sub_0017E1A0(X86Context& ctx, uint8_t* base);
extern "C" void sub_0017E1A0(X86Context& ctx, uint8_t* base) {
    game::aspect_fix::ScopedRefPixelAspect guard;
    __imp__sub_0017E1A0(ctx, base);
}

// 8. Post-FX / offscreen-copy blitters -> keep full edge-to-edge ultrawide coverage.
extern "C" void __imp__sub_00093C80(X86Context& ctx, uint8_t* base);
extern "C" void sub_00093C80(X86Context& ctx, uint8_t* base) {
    game::aspect_fix::ScopedFullScreenBypass guard;
    __imp__sub_00093C80(ctx, base);
}

extern "C" void __imp__sub_00093D32(X86Context& ctx, uint8_t* base);
extern "C" void sub_00093D32(X86Context& ctx, uint8_t* base) {
    game::aspect_fix::ScopedFullScreenBypass guard;
    __imp__sub_00093D32(ctx, base);
}

extern "C" void __imp__sub_001C23E0(X86Context& ctx, uint8_t* base);
extern "C" void sub_001C23E0(X86Context& ctx, uint8_t* base) {
    game::aspect_fix::ScopedFullScreenBypass guard;
    __imp__sub_001C23E0(ctx, base);
}
