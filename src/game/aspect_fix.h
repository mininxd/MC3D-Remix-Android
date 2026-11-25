#pragma once
#include <cstdint>

struct X86Context;

namespace game {
namespace aspect_fix {

// Guest display mode & viewport globals (XDK 5849, default.xbe).
namespace addr {
constexpr uint32_t kScreenWidth        = 0x004B4D60u;
constexpr uint32_t kScreenHeight       = 0x004B4D64u;
constexpr uint32_t kPixelAspectRatio   = 0x004B4E88u;
constexpr uint32_t kGlobalUiViewport   = 0x00558B90u;
}  // namespace addr

using GuestFunc = void (*)(X86Context&, uint8_t*);

// Keeps MC3D's pixel aspect ratio ([0x004B4E88]) synced to true host Aspect() for HOR+ 3D.
void SyncPixelAspect();

// Per-frame swap hook: handles dynamic display aspect changes and logs status once.
void OnSwap();

// Returns true iff the active D3D8 render target is the main backbuffer (not an offscreen RT).
bool IsMainBackbuffer();

// Hook helpers invoked by aspect_hooks.cpp, render_draw_hooks.cpp, and d3d8.cpp.
void AdjustOrthoArgs(X86Context& ctx);
void AdjustSetWindowArgs(X86Context& ctx);
void HandleQuadBlit(X86Context& ctx, uint8_t* base, GuestFunc orig);
void HandleFontBlit(X86Context& ctx, uint8_t* base, GuestFunc orig);
// Maps D3DRECT left/right X coords when inside ScopedMinimapClear on the main backbuffer.
// Returns true and writes mapped int32_t[4]*count rects into out_rects if remapping applied.
bool MapClearRectsIfMinimap(uint32_t count, const uint8_t* rects, uint32_t* out_rects, uint32_t max_rects);

// RAII thread-local scope guards for classifying draw & viewport contexts.
class ScopedFullScreenBypass {
public:
    ScopedFullScreenBypass();
    ~ScopedFullScreenBypass();
    ScopedFullScreenBypass(const ScopedFullScreenBypass&) = delete;
    ScopedFullScreenBypass& operator=(const ScopedFullScreenBypass&) = delete;
};

class ScopedUiOrtho {
public:
    ScopedUiOrtho();
    ~ScopedUiOrtho();
    ScopedUiOrtho(const ScopedUiOrtho&) = delete;
    ScopedUiOrtho& operator=(const ScopedUiOrtho&) = delete;
};

class ScopedMinimapSubViewport {
public:
    ScopedMinimapSubViewport();
    ~ScopedMinimapSubViewport();
    ScopedMinimapSubViewport(const ScopedMinimapSubViewport&) = delete;
    ScopedMinimapSubViewport& operator=(const ScopedMinimapSubViewport&) = delete;
};

class ScopedMinimapClear {
public:
    ScopedMinimapClear();
    ~ScopedMinimapClear();
    ScopedMinimapClear(const ScopedMinimapClear&) = delete;
    ScopedMinimapClear& operator=(const ScopedMinimapClear&) = delete;
};

class ScopedRefPixelAspect {
public:
    ScopedRefPixelAspect();
    ~ScopedRefPixelAspect();
    ScopedRefPixelAspect(const ScopedRefPixelAspect&) = delete;
    ScopedRefPixelAspect& operator=(const ScopedRefPixelAspect&) = delete;
private:
    float saved_par_ = 0.0f;
    bool active_ = false;
};

}  // namespace aspect_fix
}  // namespace game
