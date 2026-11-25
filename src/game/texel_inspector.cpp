#include "game/texel_inspector.h"

#include <algorithm>
#include <array>
#include <cstring>

#include "game/guest_mem_safe.h"
#include "game/rt_tracker.h"
#include "gpu/texture.h"
#include "kernel/memory.h"

namespace game {
namespace objlog {

namespace {

TexSampleClass ClassifyTexelPayload(const gpu::TextureDesc& d, const gpu::FormatInfo& fi, uint32_t& out_sample) {
    out_sample = 0;
    if (!d.data || d.width == 0 || d.height == 0) return TexSampleClass::OutOfRam;

    uint32_t span = gpu::LevelSize(d, 0);
    if (span == 0) span = std::max<uint32_t>(4u, d.width * d.height);
    span = std::min<uint32_t>(span, 16u << 20);

    const uint32_t stride_elem = fi.compressed ? ((fi.bits <= 4) ? 8u : 16u) : std::max<uint32_t>(4u, fi.bits / 8u);
    if (span < stride_elem) return TexSampleClass::Unknown;

    const std::array<uint32_t, 5> offsets = {
        0u,
        (span / 4u) & ~3u,
        (span / 2u) & ~3u,
        ((span * 3u) / 4u) & ~3u,
        (span - stride_elem) & ~3u,
    };

    bool any_accessible = false;
    bool all_zero = true;
    bool all_ff = true;
    bool all_rgb_zero = (fi.bits == 32 && !fi.compressed);
    bool all_equal = true;
    uint32_t first_dwords[4] = {};
    bool have_first = false;

    for (uint32_t off : offsets) {
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
    }

    if (!any_accessible) return TexSampleClass::OutOfRam;
    if (all_zero) return TexSampleClass::ZeroBlack;
    if (all_rgb_zero) return TexSampleClass::AlphaZeroBlack;
    if (all_ff) return TexSampleClass::SolidWhite;
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

    out.sample_class = ClassifyTexelPayload(d, fi, out.sample_dword);
    return out;
}

const char* SampleClassName(TexSampleClass c) {
    switch (c) {
        case TexSampleClass::Unbound: return "UNBOUND";
        case TexSampleClass::RenderTarget: return "RT";
        case TexSampleClass::InvalidDesc: return "BAD_DESC";
        case TexSampleClass::OutOfRam: return "OUT_OF_RAM";
        case TexSampleClass::ZeroBlack: return "ZERO_BLACK";
        case TexSampleClass::AlphaZeroBlack: return "RGB0_BLACK";
        case TexSampleClass::SolidWhite: return "SOLID_WHITE";
        case TexSampleClass::SolidConst: return "CONST";
        case TexSampleClass::Varied: return "VARIED";
        case TexSampleClass::Unknown: return "UNK";
    }
    return "UNK";
}

}  // namespace objlog
}  // namespace game
