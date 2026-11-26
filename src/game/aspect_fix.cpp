#include "game/aspect_fix.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

#include "common/aspect.h"
#include "core/log.h"
#include "cpu/x86_context.h"
#include "cpu/x86_ops.h"
#include "game/guest_mem_safe.h"
#include "game/render_accuracy_internal.h"
#include "game/ui_aspect_rules.h"
#include "image_symbols.h"
#include "kernel/kernel.h"

namespace game {
namespace aspect_fix {

namespace {

constexpr float kAspect16x9 = 16.0f / 9.0f;
constexpr float kAspect4x3  = 4.0f / 3.0f;
constexpr float kAspectEps  = 0.005f;

thread_local int tl_fs_bypass_depth  = 0;
thread_local int tl_ui_ortho_depth   = 0;
thread_local int tl_minimap_vp_depth = 0;
thread_local int tl_minimap_clear_depth = 0;
thread_local int tl_ref_par_depth    = 0;

std::atomic<float> g_last_logged_aspect{0.0f};
std::atomic<bool>  g_logged_ui_ortho{false};

struct AspectMetrics {
    uint32_t guest_w = 0;
    uint32_t guest_h = 0;
    float host_aspect = kAspect16x9;
    float ui_ref_aspect = kAspect16x9;
    float host_par = 1.0f;
    float ref_par = 1.0f;
    float ui_sx = 1.0f;
    float ui_x_pad = 0.0f;
    bool ultrawide_3d = false;
    bool ultrawide_ui = false;
};

float ReadGuestF32(uint32_t va, float fallback = 0.0f) {
    if (!safe_mem::CanAccess(va, 4u)) return fallback;
    float v = fallback;
    std::memcpy(&v, kernel::HostPtr(va), 4u);
    return v;
}

bool WriteGuestF32(uint32_t va, float v) {
    if (!safe_mem::CanAccess(va, 4u)) return false;
    std::memcpy(kernel::HostPtr(va), &v, 4u);
    return true;
}

AspectMetrics ComputeMetrics() {
    AspectMetrics m;
    m.guest_w = safe_mem::Read32(addr::kScreenWidth, 0u);
    m.guest_h = safe_mem::Read32(addr::kScreenHeight, 0u);
    if (m.guest_w == 0u || m.guest_h == 0u) return m;

    const float host_a = std::max(game::Aspect(), kAspect4x3);
    // MC3D chooses 720x480 with tune/ui/xboxws (16:9) vs 640x480 with tune/ui/xbox (4:3).
    const float ui_ref = (m.guest_w >= 720u) ? kAspect16x9 : kAspect4x3;
    const float hw_ratio = static_cast<float>(m.guest_h) / static_cast<float>(m.guest_w);

    m.host_aspect = host_a;
    m.ui_ref_aspect = ui_ref;
    m.host_par = hw_ratio * host_a;
    m.ref_par = hw_ratio * ui_ref;

    if (host_a > ui_ref + kAspectEps) {
        m.ultrawide_3d = true;
        // Fill full width on modern 20:9 / 21:9 displays so the UI window & menus match
        // screen width instead of narrowing into a centered 16:9 column, while 3D keeps HOR+.
        m.ui_sx = ui_aspect_rules::AdjustScaleX(1.0f, false);
        m.ui_x_pad = static_cast<float>(m.guest_w) * (1.0f - m.ui_sx) * 0.5f;
        m.ultrawide_ui = m.ui_sx < 0.999f;
    }
    return m;
}

int32_t MapXInt(int32_t x, const AspectMetrics& m) {
    return static_cast<int32_t>(std::lround(m.ui_x_pad + static_cast<float>(x) * m.ui_sx));
}

int16_t ClampI16(int32_t v) {
    constexpr int32_t kMin = std::numeric_limits<int16_t>::min();
    constexpr int32_t kMax = std::numeric_limits<int16_t>::max();
    return static_cast<int16_t>(std::clamp(v, kMin, kMax));
}

// Updates the global 2D ortho viewport ([0x00558B90]) projection X scale/offset on aspect change.
void RefreshGlobalUiOrtho(const AspectMetrics& m) {
    const uint32_t vp = safe_mem::Read32(addr::kGlobalUiViewport, 0u);
    if (!vp || !safe_mem::CanAccess(vp, 0x170u)) return;
    const uint32_t flags = safe_mem::Read32(vp + 0x12Cu, 0u);
    if ((flags & 1u) == 0u) return;  // not ortho

    const uint32_t vx = safe_mem::Read32(vp + 0x160u, 1u);
    const uint32_t vy = safe_mem::Read32(vp + 0x164u, 1u);
    const uint32_t vw = safe_mem::Read32(vp + 0x168u, 0u);
    const uint32_t vh = safe_mem::Read32(vp + 0x16Cu, 0u);
    if (vx != 0u || vy != 0u || vw != m.guest_w || vh != m.guest_h) return;

    const float m00 = (2.0f * m.ui_sx) / static_cast<float>(m.guest_w);
    const float m30 = -m.ui_sx;
    WriteGuestF32(vp + 0x04u, m00);
    WriteGuestF32(vp + 0x34u, m30);
    kernel::W32(vp + 0x124u, safe_mem::Read32(vp + 0x124u, 0u) | 2u);
}

}  // namespace

bool IsMainBackbuffer() {
    const uint32_t dev = safe_mem::Read32(render_accuracy::addr::kD3dDevicePtr, 0u);
    if (!dev) return false;
    const uint32_t rt = safe_mem::Read32(dev + XDK_CDevice_RenderTarget, 0u);
    const uint32_t bb = safe_mem::Read32(dev + XDK_CDevice_BackBuffer, 0u);
    return rt != 0u && bb != 0u && rt == bb;
}

void SyncPixelAspect() {
    if (tl_ref_par_depth > 0) return;
    const AspectMetrics m = ComputeMetrics();
    if (m.guest_w == 0u || m.guest_h == 0u) return;

    const float cur = ReadGuestF32(addr::kPixelAspectRatio, 0.0f);
    if (std::fabs(cur - m.host_par) > 1e-4f) {
        WriteGuestF32(addr::kPixelAspectRatio, m.host_par);
    }
}

void OnSwap() {
    const AspectMetrics m = ComputeMetrics();
    if (m.guest_w == 0u || m.guest_h == 0u) return;

    SyncPixelAspect();

    const float prev = g_last_logged_aspect.load(std::memory_order_relaxed);
    if (std::fabs(prev - m.host_aspect) > 0.002f) {
        g_last_logged_aspect.store(m.host_aspect, std::memory_order_relaxed);
        RefreshGlobalUiOrtho(m);
        LOGR("[OK] aspect_fix: guest=%ux%u hostAspect=%.4f 3dPAR=%.4f uiRef=%.4f uiScaleX=%.4f uiPadX=%.1f fmv=native",
             m.guest_w, m.guest_h, m.host_aspect, m.host_par, m.ui_ref_aspect,
             m.ui_sx, m.ui_x_pad);
    }
}

void AdjustOrthoArgs(X86Context& ctx) {
    if (tl_ui_ortho_depth <= 0) return;
    const AspectMetrics m = ComputeMetrics();
    if (!m.ultrawide_ui) return;

    // sub_002D4D30(__thiscall ret 0x18): [esp+4]=left, [esp+8]=right, [esp+12]=bottom, [esp+16]=top
    const float left   = ReadGuestF32(ctx.esp + 0x04u, -1.0f);
    const float right  = ReadGuestF32(ctx.esp + 0x08u, -1.0f);
    const float bottom = ReadGuestF32(ctx.esp + 0x0Cu, -1.0f);
    const float top    = ReadGuestF32(ctx.esp + 0x10u, -1.0f);

    if (std::fabs(left) < 0.5f && std::fabs(top) < 0.5f &&
        std::fabs(right - static_cast<float>(m.guest_w)) < 0.5f &&
        std::fabs(bottom - static_cast<float>(m.guest_h)) < 0.5f) {
        const float pad_ortho = static_cast<float>(m.guest_w) * (1.0f - m.ui_sx) / (2.0f * m.ui_sx);
        WriteGuestF32(ctx.esp + 0x04u, -pad_ortho);
        WriteGuestF32(ctx.esp + 0x08u, static_cast<float>(m.guest_w) + pad_ortho);

        if (!g_logged_ui_ortho.exchange(true, std::memory_order_relaxed)) {
            LOGR("[OK] aspect_fix: 2D UI/Flash ortho pillarboxed to [%.1f .. %.1f] (sx=%.4f)",
                 -pad_ortho, static_cast<float>(m.guest_w) + pad_ortho, m.ui_sx);
        }
    }
}

void AdjustSetWindowArgs(X86Context& ctx) {
    SyncPixelAspect();
    if (tl_minimap_vp_depth <= 0) return;
    const AspectMetrics m = ComputeMetrics();
    if (!m.ultrawide_ui) return;

    // sub_002D4F10(__thiscall ret 0x18): [esp+4]=x, [esp+8]=y, [esp+12]=w, [esp+16]=h
    if (!safe_mem::CanAccess(ctx.esp + 0x04u, 16u)) return;
    const int32_t orig_x = static_cast<int32_t>(kernel::R32(ctx.esp + 0x04u));
    const int32_t orig_w = static_cast<int32_t>(kernel::R32(ctx.esp + 0x0Cu));
    if (orig_w <= 0) return;

    const int32_t new_x0 = MapXInt(orig_x, m);
    const int32_t new_x1 = MapXInt(orig_x + orig_w, m);
    const int32_t new_w  = std::max<int32_t>(1, new_x1 - new_x0);
    kernel::W32(ctx.esp + 0x04u, static_cast<uint32_t>(new_x0));
    kernel::W32(ctx.esp + 0x0Cu, static_cast<uint32_t>(new_w));
}

void HandleQuadBlit(X86Context& ctx, uint8_t* base, GuestFunc orig) {
    if (!IsMainBackbuffer() || !safe_mem::CanAccess(ctx.esp + 0x04u, 16u)) {
        orig(ctx, base);
        return;
    }

    const AspectMetrics m = ComputeMetrics();
    if (m.guest_w == 0u || m.guest_h == 0u) {
        orig(ctx, base);
        return;
    }

    const uint32_t arg_base = ctx.esp + 0x04u;
    const uint32_t saved_x0 = kernel::R32(arg_base + 0u);
    const uint32_t saved_y0 = kernel::R32(arg_base + 4u);
    const uint32_t saved_x1 = kernel::R32(arg_base + 8u);
    const uint32_t saved_y1 = kernel::R32(arg_base + 12u);

    if (tl_fs_bypass_depth <= 0 && m.ultrawide_ui) {
        const int32_t sx0 = static_cast<int32_t>(saved_x0);
        const int32_t sy0 = static_cast<int32_t>(saved_y0);
        const int32_t sx1 = static_cast<int32_t>(saved_x1);
        const int32_t sy1 = static_cast<int32_t>(saved_y1);

        // Preserve full-screen fades and menu backdrop covers edge-to-edge.
        if (ui_aspect_rules::ShouldKeepFullScreenQuad(sx0, sy0, sx1, sy1, m.guest_w, m.guest_h)) {
            orig(ctx, base);
            return;
        }

        const int32_t nx0 = MapXInt(sx0, m);
        const int32_t nx1 = MapXInt(sx1, m);
        kernel::W32(arg_base + 0u, static_cast<uint32_t>(nx0));
        kernel::W32(arg_base + 8u, static_cast<uint32_t>(nx1));

        orig(ctx, base);

        kernel::W32(arg_base + 0u, saved_x0);
        kernel::W32(arg_base + 8u, saved_x1);
        return;
    }

    orig(ctx, base);
}

void HandleFontBlit(X86Context& ctx, uint8_t* base, GuestFunc orig) {
    const AspectMetrics m = ComputeMetrics();
    if (!m.ultrawide_ui || !IsMainBackbuffer() || !safe_mem::CanAccess(ctx.esp + 0x04u, 16u)) {
        orig(ctx, base);
        return;
    }

    // sub_002D1F00(float* pos, int16_t* quads, uint8_t* uvs, uint32_t count, uint32_t color, uint8_t flag)
    const uint32_t pos_va   = kernel::R32(ctx.esp + 0x04u);
    const uint32_t quads_va = kernel::R32(ctx.esp + 0x08u);
    const uint32_t count    = kernel::R32(ctx.esp + 0x10u);
    if (count == 0u || count > 4096u ||
        !safe_mem::CanAccess(pos_va, 12u) ||
        !safe_mem::CanAccess(quads_va, count * 8u)) {
        orig(ctx, base);
        return;
    }

    const float orig_pos_x = ReadGuestF32(pos_va, 0.0f);
    WriteGuestF32(pos_va, m.ui_x_pad + orig_pos_x * m.ui_sx);

    int16_t* guest_quads = reinterpret_cast<int16_t*>(kernel::HostPtr(quads_va));
    constexpr uint32_t kStackGlyphs = 256u;
    int16_t stack_saved[kStackGlyphs * 4u];
    std::vector<int16_t> heap_saved;
    int16_t* saved = stack_saved;
    if (count > kStackGlyphs) {
        heap_saved.resize(static_cast<size_t>(count) * 4u);
        saved = heap_saved.data();
    }
    std::memcpy(saved, guest_quads, static_cast<size_t>(count) * 8u);

    for (uint32_t i = 0; i < count; ++i) {
        const int32_t dx = saved[i * 4u + 0u];
        const int32_t dw = saved[i * 4u + 2u];
        const int32_t nx0 = static_cast<int32_t>(std::lround(static_cast<float>(dx) * m.ui_sx));
        const int32_t nx1 = static_cast<int32_t>(std::lround(static_cast<float>(dx + dw) * m.ui_sx));
        guest_quads[i * 4u + 0u] = ClampI16(nx0);
        guest_quads[i * 4u + 2u] = ClampI16(nx1 - nx0);
    }

    orig(ctx, base);

    std::memcpy(guest_quads, saved, static_cast<size_t>(count) * 8u);
    WriteGuestF32(pos_va, orig_pos_x);
}

bool MapClearRectsIfMinimap(uint32_t count, const uint8_t* rects, uint32_t* out_rects, uint32_t max_rects) {
    if (tl_minimap_clear_depth <= 0 || !rects || !out_rects || count == 0u || count > max_rects) return false;
    if (!IsMainBackbuffer()) return false;
    const AspectMetrics m = ComputeMetrics();
    if (!m.ultrawide_ui || m.guest_w == 0u) return false;

    for (uint32_t i = 0u; i < count; ++i) {
        int32_t r[4];
        std::memcpy(r, rects + static_cast<size_t>(i) * sizeof(r), sizeof(r));
        const int32_t l = std::max<int32_t>(0, MapXInt(r[0], m) - 1);
        const int32_t ri = std::min<int32_t>(static_cast<int32_t>(m.guest_w), MapXInt(r[2], m) + 1);
        r[0] = l;
        r[2] = std::max<int32_t>(l, ri);
        std::memcpy(out_rects + static_cast<size_t>(i) * 4u, r, sizeof(r));
    }
    return true;
}

// ============================================================================
// RAII Scope Guards
// ============================================================================

ScopedFullScreenBypass::ScopedFullScreenBypass() { ++tl_fs_bypass_depth; }
ScopedFullScreenBypass::~ScopedFullScreenBypass() { --tl_fs_bypass_depth; }

ScopedUiOrtho::ScopedUiOrtho() { ++tl_ui_ortho_depth; }
ScopedUiOrtho::~ScopedUiOrtho() { --tl_ui_ortho_depth; }

ScopedMinimapSubViewport::ScopedMinimapSubViewport() { ++tl_minimap_vp_depth; }
ScopedMinimapSubViewport::~ScopedMinimapSubViewport() { --tl_minimap_vp_depth; }

ScopedMinimapClear::ScopedMinimapClear() { ++tl_minimap_clear_depth; }
ScopedMinimapClear::~ScopedMinimapClear() { --tl_minimap_clear_depth; }

ScopedRefPixelAspect::ScopedRefPixelAspect() {
    ++tl_ref_par_depth;
    const AspectMetrics m = ComputeMetrics();
    if (m.ultrawide_3d) {
        saved_par_ = ReadGuestF32(addr::kPixelAspectRatio, m.host_par);
        WriteGuestF32(addr::kPixelAspectRatio, m.ref_par);
        active_ = true;
    }
}

ScopedRefPixelAspect::~ScopedRefPixelAspect() {
    if (active_) {
        WriteGuestF32(addr::kPixelAspectRatio, saved_par_);
    }
    --tl_ref_par_depth;
}

}  // namespace aspect_fix
}  // namespace game
