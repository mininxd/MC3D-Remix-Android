#include "game/texel_inspector.h"

#include <algorithm>
#include <array>
#include <cstring>

#include "game/guest_mem_safe.h"
#include "game/p8_palette_probe.h"
#include "game/rt_tracker.h"
#include "gpu/texture.h"
#include "kernel/memory.h"

namespace game {
namespace objlog {

namespace {

constexpr uint32_t kProbeCount = 25u;

// Generates 25 well-distributed element-aligned offsets across [0, span - elem] so sparse
// UI icons, font sheets, and DXT5 decals sample interior artwork instead of only margins.
uint32_t ComputeProbeOffset(uint32_t idx, uint32_t num_elems, uint32_t elem_bytes) {
    if (num_elems <= 1u) return 0u;
    // Mix interior fractions (avoiding pure power-of-2 corners) across the full buffer.
    const uint64_t num = uint64_t(idx) * 2u + 1u;
    const uint64_t elem_idx = (num * uint64_t(num_elems - 1u)) / (uint64_t(kProbeCount) * 2u);
    return static_cast<uint32_t>(std::min<uint64_t>(elem_idx, num_elems - 1u)) * elem_bytes;
}

TexSampleClass ClassifyP8WithPalette(const gpu::TextureDesc& d, uint32_t span,
                                     const std::array<uint32_t, 256>& pal, uint32_t& out_sample) {
    const uint32_t num_dwords = span / 4u;
    if (num_dwords == 0u) return TexSampleClass::Unknown;

    bool any_accessible = false;
    bool all_transparent = true;
    bool all_rgb_zero = true;
    bool all_white_opaque = true;
    bool any_colored_visible = false;
    bool any_non_opaque = false;
    bool all_equal = true;
    uint32_t first_bgra = 0u;
    bool have_first = false;

    for (uint32_t i = 0; i < kProbeCount; ++i) {
        const uint32_t off = ComputeProbeOffset(i, num_dwords, 4u);
        const uint32_t va = d.data + off;
        if (!safe_mem::CanAccess(va, 4u)) continue;
        any_accessible = true;
        const uint32_t packed = safe_mem::Read32(va, 0u);

        for (uint32_t b = 0; b < 4u; ++b) {
            const uint32_t idx = (packed >> (b * 8u)) & 0xFFu;
            const uint32_t bgra = pal[idx];
            const uint32_t alpha = (bgra >> 24u) & 0xFFu;
            const uint32_t rgb = bgra & 0x00FFFFFFu;
            if (!have_first) {
                first_bgra = bgra;
                out_sample = bgra;
                have_first = true;
            } else if (bgra != first_bgra) {
                all_equal = false;
            }
            if (alpha != 0u) {
                all_transparent = false;
                if (rgb != 0x00FFFFFFu) any_colored_visible = true;
            }
            if (alpha != 0xFFu) any_non_opaque = true;
            if (rgb != 0u) all_rgb_zero = false;
            if (bgra != 0xFFFFFFFFu) all_white_opaque = false;
        }
        if (!all_equal && !all_transparent && !all_rgb_zero && !all_white_opaque && any_colored_visible) {
            return TexSampleClass::Varied;
        }
    }

    if (!any_accessible) return TexSampleClass::OutOfRam;
    if (all_transparent) return TexSampleClass::TransparentZero;
    if (all_rgb_zero) return TexSampleClass::ZeroBlack;
    if (all_white_opaque) return TexSampleClass::SolidWhite;
    if (!any_colored_visible && any_non_opaque) return TexSampleClass::WhiteRgbAlphaMask;
    if (all_equal) return TexSampleClass::SolidConst;
    return TexSampleClass::Varied;
}

TexSampleClass ClassifyTexelPayload(const gpu::TextureDesc& d, const gpu::FormatInfo& fi,
                                    uint32_t pal_va, uint32_t& out_sample) {
    out_sample = 0;
    if (!d.data || d.width == 0 || d.height == 0) return TexSampleClass::OutOfRam;

    uint32_t span = gpu::LevelSize(d, 0);
    if (span == 0) span = std::max<uint32_t>(4u, d.width * d.height);
    span = std::min<uint32_t>(span, 16u << 20);

    if (d.format == 0x0Bu && pal_va != 0u) {
        std::array<uint32_t, 256> pal{};
        if (ReadXboxPalette(pal_va, pal)) {
            return ClassifyP8WithPalette(d, span, pal, out_sample);
        }
    }

    const uint32_t stride_elem = fi.compressed ? ((fi.bits <= 4) ? 8u : 16u) : std::max<uint32_t>(4u, fi.bits / 8u);
    if (span < stride_elem) return TexSampleClass::Unknown;
    const uint32_t num_elems = span / stride_elem;

    const bool is_a8 = (d.format == 0x19u || d.format == 0x1Fu);
    const bool is_argb32 = (d.format == 0x06u || d.format == 0x12u);
    const bool is_dxt_alpha = (d.format == 0x0Eu || d.format == 0x0Fu);

    bool any_accessible = false;
    bool all_zero = true;
    bool all_transparent = true;
    bool all_ff = true;
    bool all_rgb_zero = is_argb32 || (d.format == 0x3Au || d.format == 0x3Fu);
    bool all_white_alpha_mask = (is_a8 || is_argb32 || is_dxt_alpha);
    bool any_alpha_non_ff = false;
    bool all_equal = true;
    uint32_t first_dwords[4] = {};
    bool have_first = false;

    for (uint32_t p = 0; p < kProbeCount; ++p) {
        const uint32_t off = ComputeProbeOffset(p, num_elems, stride_elem);
        const uint32_t va = d.data + off;
        if (!safe_mem::CanAccess(va, stride_elem)) continue;
        any_accessible = true;

        const uint32_t nwords = std::min<uint32_t>(4u, std::max<uint32_t>(1u, stride_elem / 4u));
        uint32_t cur[4] = {};
        for (uint32_t i = 0; i < nwords; ++i) {
            cur[i] = safe_mem::Read32(va + i * 4u, 0);
            if (cur[i] != 0u) all_zero = false;
            if (cur[i] != 0xFFFFFFFFu) all_ff = false;
        }
        bool block_transparent = false;
        if (fi.compressed) {
            block_transparent = IsCompressedBlockTransparent(d.format, cur[0], nwords > 1u ? cur[1] : 0u);
        } else {
            block_transparent = IsUncompressedDwordTransparent(d.format, cur[0]);
        }
        if (!block_transparent) all_transparent = false;

        if (all_white_alpha_mask) {
            if (is_a8) {
                if (cur[0] != 0xFFFFFFFFu) any_alpha_non_ff = true;
            } else if (is_argb32) {
                if ((cur[0] >> 24u) != 0xFFu) any_alpha_non_ff = true;
                if (!block_transparent && (cur[0] & 0x00FFFFFFu) != 0x00FFFFFFu) all_white_alpha_mask = false;
            } else if (is_dxt_alpha && nwords >= 3u) {
                if (d.format == 0x0Eu && (cur[0] != 0xFFFFFFFFu || cur[1] != 0xFFFFFFFFu)) any_alpha_non_ff = true;
                if (d.format == 0x0Fu && (cur[0] & 0xFFFFu) != 0xFFFFu) any_alpha_non_ff = true;
                if (!block_transparent && cur[2] != 0xFFFFFFFFu) all_white_alpha_mask = false;
            }
        }

        if (!have_first) {
            for (uint32_t i = 0; i < nwords; ++i) first_dwords[i] = cur[i];
            have_first = true;
            out_sample = cur[0];
        } else {
            for (uint32_t i = 0; i < nwords; ++i) {
                if (cur[i] != first_dwords[i]) {
                    all_equal = false;
                    break;
                }
            }
        }
        if (all_rgb_zero && (cur[0] & 0x00FFFFFFu) != 0u) all_rgb_zero = false;
        if (!all_equal && !all_zero && !all_transparent && !all_ff && !all_rgb_zero && !all_white_alpha_mask) {
            return TexSampleClass::Varied;
        }
    }

    if (!any_accessible) return TexSampleClass::OutOfRam;
    if (all_transparent) return TexSampleClass::TransparentZero;
    if (all_zero) return TexSampleClass::ZeroBlack;
    if (all_rgb_zero) return TexSampleClass::AlphaZeroBlack;
    if (all_ff) return TexSampleClass::SolidWhite;
    if (all_white_alpha_mask && any_alpha_non_ff) return TexSampleClass::WhiteRgbAlphaMask;
    if (all_equal) return TexSampleClass::SolidConst;
    return TexSampleClass::Varied;
}

}  // namespace

StageTexInspection InspectStageTexture(const gpu::State& s, uint32_t stage) {
    StageTexInspection out{};
    if (stage >= 4u) return out;
    out.bound_va = s.textures[stage];
    out.pal_va = s.palettes[stage];
    if (!out.bound_va) {
        out.sample_class = TexSampleClass::Unbound;
        return out;
    }

    gpu::TextureDesc d{};
    if (!safe_mem::CanAccess(out.bound_va, 20u) || !gpu::Describe(out.bound_va, d)) {
        out.sample_class = TexSampleClass::InvalidDesc;
        out.fmt_name = "BAD_DESC";
        return out;
    }

    const gpu::FormatInfo& fi = gpu::Format(d.format);
    out.valid_desc = true;
    out.data_va = d.data;
    out.width = d.width;
    out.height = d.height;
    out.levels = d.levels;
    out.format = d.format;
    out.fmt_name = fi.name ? fi.name : "UNK";
    out.cube = d.cube;
    out.linear = d.linear;
    out.is_p8 = (fi.name && std::strcmp(fi.name, "P8") == 0);
    out.p8_no_pal = out.is_p8 && (out.pal_va == 0u);

    const uint32_t active_rt_va = s.rt.data ? mem::VirtualFromPhysical(s.rt.data) : 0u;
    const uint32_t active_zs_va = s.zs.data ? mem::VirtualFromPhysical(s.zs.data) : 0u;
    if (d.data != 0u && (d.data == active_rt_va || (active_zs_va != 0u && d.data == active_zs_va))) {
        out.rt_feedback = true;
        out.sample_class = TexSampleClass::RenderTarget;
        return out;
    }
    if (d.data != 0u && rt_tracker::IsKnownRenderTarget(d.data)) {
        out.sample_class = TexSampleClass::RenderTarget;
        return out;
    }

    out.sample_class = ClassifyTexelPayload(d, fi, out.pal_va, out.sample_dword);
    return out;
}

const char* SampleClassName(TexSampleClass c) {
    switch (c) {
        case TexSampleClass::Unbound: return "UNBOUND";
        case TexSampleClass::RenderTarget: return "RT";
        case TexSampleClass::TransparentZero: return "TRANSPARENT";
        case TexSampleClass::InvalidDesc: return "BAD_DESC";
        case TexSampleClass::OutOfRam: return "OUT_OF_RAM";
        case TexSampleClass::ZeroBlack: return "ZERO_BLACK";
        case TexSampleClass::AlphaZeroBlack: return "RGB0_BLACK";
        case TexSampleClass::SolidWhite: return "SOLID_WHITE";
        case TexSampleClass::WhiteRgbAlphaMask: return "WHITE_ALPHA_MASK";
        case TexSampleClass::SolidConst: return "CONST";
        case TexSampleClass::Varied: return "VARIED";
        case TexSampleClass::Unknown: return "UNK";
    }
    return "UNK";
}

}  // namespace objlog
}  // namespace game
