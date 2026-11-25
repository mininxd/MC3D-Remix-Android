#include "game/effect_probe.h"

#include <algorithm>
#include <cstdio>

#include "game/d3d_guest_sync.h"
#include "game/guest_mem_safe.h"
#include "game/render_accuracy_internal.h"
#include "gpu/vertex.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

namespace game {
namespace objlog {

namespace {

// MC3D immediate push buffer state (verified from sub_002D5D90 / sub_002D5E40).
constexpr uint32_t kMcImmEnd112   = 0x00559A68u;
constexpr uint32_t kMcImmEnd142   = 0x00559A6Cu;
constexpr uint32_t kMcImmHdrVa    = 0x00559A7Cu;
constexpr uint32_t kMcImmDiffuse  = 0x00559A84u;
constexpr uint32_t kMcImmFvf      = 0x00559A90u;
constexpr uint32_t kMcImmEnd152   = 0x00559A98u;
constexpr uint32_t kXdkResDataOff = 4u;

thread_local const char* tl_effect_name = nullptr;
thread_local VertexProbeInfo tl_vprobe{};
thread_local std::string tl_pre_sync_desync;

void AccumulateSample(VertexProbeInfo& info, uint32_t argb, uint32_t idx) {
    if (idx == 0u) {
        info.first_argb = argb;
        info.last_argb = argb;
        info.all_white_rgb = ((argb & 0x00FFFFFFu) == 0x00FFFFFFu);
        info.all_alpha_zero = ((argb >> 24u) == 0u);
        info.all_opaque = ((argb >> 24u) == 0xFFu);
    } else {
        info.last_argb = argb;
        if (argb != info.first_argb) info.varied = true;
        if ((argb & 0x00FFFFFFu) != 0x00FFFFFFu) info.all_white_rgb = false;
        if ((argb >> 24u) != 0u) info.all_alpha_zero = false;
        if ((argb >> 24u) != 0xFFu) info.all_opaque = false;
    }
    info.has_vcol = true;
}

const char* BlendName(uint32_t v) {
    switch (v) {
        case 0u:      return "0";
        case 1u:      return "1";
        case 0x300u:  return "SC";
        case 0x301u:  return "ISC";
        case 0x302u:  return "SA";
        case 0x303u:  return "ISA";
        case 0x304u:  return "DA";
        case 0x305u:  return "IDA";
        case 0x306u:  return "DC";
        case 0x307u:  return "IDC";
        case 0x308u:  return "SAS";
        case 0x8001u: return "CC";
        case 0x8002u: return "ICC";
        case 0x8003u: return "CA";
        case 0x8004u: return "ICA";
        default:      return "?";
    }
}

}  // namespace

ScopedEffectContext::ScopedEffectContext(const char* name) : prev_(tl_effect_name) {
    tl_effect_name = name;
}

ScopedEffectContext::~ScopedEffectContext() {
    tl_effect_name = prev_;
}

const char* ActiveEffectName() {
    return tl_effect_name;
}

ScopedVertexProbe::ScopedVertexProbe() : prev_(tl_vprobe) {
    tl_vprobe = VertexProbeInfo{};
}

ScopedVertexProbe::~ScopedVertexProbe() {
    tl_vprobe = prev_;
}

void ProbeImmediatePushColors() {
    VertexProbeInfo info{};
    info.from_imm_push = true;
    const uint32_t fvf = safe_mem::Read32(kMcImmFvf, 0u);
    if (fvf != 0u) info.effective_fvf = fvf;

    uint32_t stride = 0u;
    uint32_t diff_off = 0u;
    uint32_t end_va = 0u;
    if (fvf == 0x142u) {
        stride = 24u;
        diff_off = 12u;
        end_va = safe_mem::Read32(kMcImmEnd142, 0u);
    } else if (fvf == 0x152u) {
        stride = 36u;
        diff_off = 24u;
        end_va = safe_mem::Read32(kMcImmEnd152, 0u);
    } else {
        (void)kMcImmEnd112;
    }

    const uint32_t hdr_va = safe_mem::Read32(kMcImmHdrVa, 0u);
    if (stride != 0u && hdr_va != 0u && end_va > hdr_va + 4u && ((end_va - (hdr_va + 4u)) % stride) == 0u) {
        const uint32_t start = hdr_va + 4u;
        const uint32_t count = (end_va - start) / stride;
        info.vert_count = count;
        const uint32_t lim = std::min<uint32_t>(count, 32u);
        for (uint32_t i = 0u; i < lim; ++i) {
            const uint32_t va = start + i * stride + diff_off;
            if (!safe_mem::CanAccess(va, 4u)) break;
            AccumulateSample(info, kernel::R32(va), i);
        }
    }

    if (!info.has_vcol && (fvf & 0x40u) != 0u) {
        const uint32_t cur_diff = safe_mem::Read32(kMcImmDiffuse, 0u);
        AccumulateSample(info, cur_diff, 0u);
        info.vert_count = 1u;
    }

    tl_vprobe = info;
}

void ProbeParticleColorArray(uint32_t color_arr_va, int32_t count) {
    VertexProbeInfo info{};
    info.from_particle_arr = true;
    info.effective_fvf = 0x144u;  // sub_002D6830 immediately binds FVF 0x144 before drawing.
    if (color_arr_va != 0u && count > 0) {
        const uint32_t ucount = static_cast<uint32_t>(count);
        info.vert_count = ucount;
        const uint32_t lim = std::min<uint32_t>(ucount, 64u);
        for (uint32_t i = 0u; i < lim; ++i) {
            const uint32_t va = color_arr_va + i * 4u;
            if (!safe_mem::CanAccess(va, 4u)) break;
            AccumulateSample(info, kernel::R32(va), i);
        }
    }
    tl_vprobe = info;
}

VertexProbeInfo ResolveVertexProbe(const gpu::State& s) {
    if (tl_vprobe.has_vcol || tl_vprobe.effective_fvf != 0u) {
        return tl_vprobe;
    }

    VertexProbeInfo out{};
    const uint32_t eff_vs = (s.vs_handle != 0u) ? s.vs_handle : tl_vprobe.effective_fvf;
    gpu::VertexShader fvf_vs{};
    const gpu::VertexShader* vs = s.vs.get();
    if (!vs && eff_vs != 0u && !(eff_vs & 1u)) {
        gpu::FvfInputs(eff_vs, fvf_vs);
        vs = &fvf_vs;
    }
    if (!vs || vs->inputs[3].stream < 0 || vs->inputs[3].type != 0x40u) {
        return out;
    }

    const uint32_t st = static_cast<uint32_t>(vs->inputs[3].stream);
    if (st >= 16u) return out;
    const uint32_t vb = s.streams[st].vb;
    const uint32_t stride = s.streams[st].stride;
    const uint32_t off = vs->inputs[3].offset;
    if (!vb || stride < off + 4u || !safe_mem::CanAccess(vb, 8u)) return out;

    const uint32_t phys = safe_mem::Read32(vb + kXdkResDataOff, 0u);
    if (!phys) return out;
    const uint32_t data_va = mem::VirtualFromPhysical(phys) + s.base_vertex * stride;

    out.from_vb0 = true;
    for (uint32_t i = 0u; i < 16u; ++i) {
        const uint32_t va = data_va + i * stride + off;
        if (!safe_mem::CanAccess(va, 4u)) break;
        AccumulateSample(out, kernel::R32(va), i);
        ++out.vert_count;
    }
    return out;
}

void CapturePreSyncSimpleDesync(const gpu::State& s) {
    tl_pre_sync_desync.clear();
    if (s.ps != nullptr) return;

    char buf[192];
    uint32_t count = 0u;
    for (uint32_t rs = 57u; rs < gpu::kRsDeferredFirst; ++rs) {
        const uint32_t method = d3d_sync::SimpleMethodForRs(rs);
        if (!method || (method >> 2) >= s.simple.size()) continue;
        const uint32_t ea = render_accuracy::addr::kD3dRenderState + rs * 4u;
        if (!safe_mem::CanAccess(ea, 4u)) continue;
        const uint32_t guest_val = kernel::R32(ea);
        const uint32_t host_val = s.simple[method >> 2];
        if (guest_val != host_val) {
            if (count < 3u) {
                std::snprintf(buf, sizeof(buf), "%srs%u(m=0x%X):g=0x%X!=h=0x%X",
                              tl_pre_sync_desync.empty() ? "" : ",", rs, method, guest_val, host_val);
                tl_pre_sync_desync += buf;
            }
            ++count;
        }
    }
}

bool ConsumeSimpleRsDesync(std::string& out_detail) {
    if (tl_pre_sync_desync.empty()) return false;
    out_detail = tl_pre_sync_desync;
    tl_pre_sync_desync.clear();
    return true;
}

std::string FormatBlendAndEffectSuffix(const gpu::State& s, const VertexProbeInfo& vp, const char* eff_name) {
    const uint32_t b_en  = s.simple[gpu::kMBlendEnable / 4u] & 1u;
    const uint32_t b_src = s.simple[gpu::kMSrcBlend / 4u];
    const uint32_t b_dst = s.simple[gpu::kMDestBlend / 4u];
    const uint32_t b_op  = s.simple[gpu::kMBlendOp / 4u];
    const uint32_t at_en = s.simple[gpu::kMAlphaTestEnable / 4u] & 1u;
    const uint32_t at_fn = s.simple[gpu::kMAlphaFunc / 4u] & 0xFFFu;
    const uint32_t at_rf = s.simple[gpu::kMAlphaRef / 4u] & 0xFFu;
    const uint32_t cw    = s.simple[gpu::kMColorWriteEnable / 4u];
    const uint32_t fog   = s.rs[gpu::kRsFogEnable] & 1u;
    const uint32_t fogc  = s.rs[gpu::kRsFogColor];

    std::string out;
    char buf[384];
    std::snprintf(buf, sizeof(buf),
                  " blend=%u(%s->%s,op=0x%X) atest=%u(fn=0x%X,ref=%u) cwrite=0x%X fog=%u(0x%08X)",
                  b_en, BlendName(b_src), BlendName(b_dst), b_op,
                  at_en, at_fn, at_rf, cw, fog, fogc);
    out += buf;

    if (!s.ps) {
        std::snprintf(buf, sizeof(buf), " aa1=(%u,%u) aa2=(%u,%u)",
                      s.tss[0][gpu::kTssAlphaArg1], s.tss[1][gpu::kTssAlphaArg1],
                      s.tss[0][gpu::kTssAlphaArg2], s.tss[1][gpu::kTssAlphaArg2]);
        out += buf;
    }

    if (vp.has_vcol) {
        const char* src_tag = vp.from_particle_arr ? "ptcl" : (vp.from_imm_push ? "imm" : "vb0");
        std::snprintf(buf, sizeof(buf), " vcol=(%s n=%u 0x%08X%s)",
                      src_tag, vp.vert_count, vp.first_argb, vp.varied ? "+" : "");
        out += buf;
    }
    if (vp.effective_fvf != 0u && s.vs_handle == 0u) {
        std::snprintf(buf, sizeof(buf), " effFvf=0x%X", vp.effective_fvf);
        out += buf;
    }
    if (eff_name && eff_name[0]) {
        std::snprintf(buf, sizeof(buf), " eff=%s", eff_name);
        out += buf;
    }
    return out;
}

}  // namespace objlog
}  // namespace game
