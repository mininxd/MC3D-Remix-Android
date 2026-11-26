#include "game/object_anomaly.h"

#include "game/vk_format_probe.h"
#include "gpu/vertex.h"

namespace game {
namespace objlog {

namespace {

constexpr uint32_t kTopDisable     = 1u;
constexpr uint32_t kTopSelectArg1  = 2u;
constexpr uint32_t kTopSelectArg2  = 3u;
constexpr uint32_t kTopModulate    = 4u;
constexpr uint32_t kTaMask         = 0x0Fu;
constexpr uint32_t kTaDiffuse      = 0u;
constexpr uint32_t kTaTexture      = 2u;
constexpr uint32_t kTaTfactor      = 3u;
constexpr uint32_t kTaTemp         = 5u;

void AppendTag(std::string& dst, const char* tag) {
    if (!dst.empty()) dst.push_back('|');
    dst += tag;
}

uint16_t CollectInputRegMask(const gpu::State& s, uint32_t eff_vs,
                              bool& out_vs_prog, bool& out_xyzrhw, bool& out_bgra_vcol) {
    out_vs_prog = (eff_vs & 1u) != 0 && s.vs && !s.vs->program.empty();
    out_xyzrhw = !(eff_vs & 1u) && ((eff_vs & 0x0Eu) == 0x04u);
    out_bgra_vcol = false;

    const gpu::VertexShader* active_vs = s.vs.get();
    gpu::VertexShader fvf_vs{};
    if (!active_vs && eff_vs != 0 && !(eff_vs & 1u)) {
        gpu::FvfInputs(eff_vs, fvf_vs);
        active_vs = &fvf_vs;
    }

    uint16_t mask = 0;
    if (active_vs) {
        for (int r = 0; r < 16; ++r) {
            if (active_vs->inputs[r].stream >= 0) {
                mask |= static_cast<uint16_t>(1u << r);
                if (active_vs->inputs[r].type == 0x40u) out_bgra_vcol = true;
            }
        }
    }
    return mask;
}

uint32_t ComputeFfReadMask(const gpu::State& s, bool& out_temp_unwritten) {
    uint32_t mask = 0;
    bool temp_written = false;
    out_temp_unwritten = false;

    for (uint32_t st = 0; st < 4u; ++st) {
        const uint32_t cop = s.tss[st][gpu::kTssColorOp];
        const uint32_t aop = s.tss[st][gpu::kTssAlphaOp];
        if (cop <= kTopDisable) break;

        auto inspect_op = [&](uint32_t op, uint32_t a0, uint32_t a1, uint32_t a2) {
            if (op <= kTopDisable) return;
            auto check_arg = [&](uint32_t a) {
                const uint32_t src = a & kTaMask;
                if (src == kTaTexture) mask |= (1u << st);
                if (src == kTaTemp && !temp_written) out_temp_unwritten = true;
            };
            check_arg(a1);
            if (op > kTopSelectArg1) check_arg(a2);
            if (op == 25u || op == 26u) check_arg(a0);  // MULTIPLYADD / LERP
        };

        inspect_op(cop, s.tss[st][gpu::kTssColorArg0], s.tss[st][gpu::kTssColorArg1],
                   s.tss[st][gpu::kTssColorArg2]);
        inspect_op(aop, s.tss[st][gpu::kTssAlphaArg0], s.tss[st][gpu::kTssAlphaArg1],
                   s.tss[st][gpu::kTssAlphaArg2]);

        if (s.tss[st][gpu::kTssResultArg] == kTaTemp) temp_written = true;
    }
    return mask;
}

}  // namespace

AnomalyReport AnalyzeDrawAnomalies(const gpu::State& s, const std::array<StageTexInspection, 4>& stages,
                                   uint32_t ps_read_mask, const VertexProbeInfo& vp, const char* eff_name) {
    AnomalyReport rep{};
    rep.eff_vs_handle = (s.vs_handle != 0u) ? s.vs_handle : vp.effective_fvf;
    bool xyzrhw = false;
    bool has_bgra_vcol = false;
    rep.vs_regs_mask = CollectInputRegMask(s, rep.eff_vs_handle, rep.vs_prog, xyzrhw, has_bgra_vcol);
    rep.is_3d = !xyzrhw;
    if (vp.has_vcol) has_bgra_vcol = true;

    if ((rep.eff_vs_handle != 0 || s.vs != nullptr) && rep.vs_regs_mask == 0) {
        rep.flags |= kAnomBadVsNoRegs;
        AppendTag(rep.summary, "BAD_VS_NO_REGS");
    }

    if (has_bgra_vcol && !IsBgra8VertexSupported()) {
        rep.flags |= kAnomVkBgraVertexUnsupported;
        AppendTag(rep.summary, "VK_BGRA_VB_UNSUPPORTED");
    }

    std::string desync_detail;
    if (ConsumeSimpleRsDesync(desync_detail)) {
        rep.flags |= kAnomSimpleRsDesync;
        AppendTag(rep.summary, ("SIMPLE_RS_DESYNC(" + desync_detail + ")").c_str());
    }

    const bool blend_on = (s.simple[gpu::kMBlendEnable / 4u] & 1u) != 0u;
    const bool atest_on = (s.simple[gpu::kMAlphaTestEnable / 4u] & 1u) != 0u;
    const uint32_t dst_blend = s.simple[gpu::kMDestBlend / 4u];

    if (s.ps != nullptr) {
        rep.read_tex_mask = ps_read_mask;
        const uint32_t modes = s.rs[gpu::kRsPsTextureModes];
        for (uint32_t st = 0; st < 4u; ++st) {
            if (!(rep.read_tex_mask & (1u << st))) continue;
            const StageTexInspection& t = stages[st];
            const uint32_t mode = (modes >> (st * 5u)) & 0x1Fu;
            if (!t.bound_va) {
                rep.flags |= kAnomProgUnboundTex;
                AppendTag(rep.summary, "PROG_UNBOUND_TEX");
            } else if (mode == 0u) {
                rep.flags |= kAnomProgMode0Tex;
                AppendTag(rep.summary, "PROG_MODE0_TEX");
            }
        }
    } else {
        bool temp_unwritten = false;
        rep.read_tex_mask = ComputeFfReadMask(s, temp_unwritten);
        if (temp_unwritten) {
            rep.flags |= kAnomFfTempUnwritten;
            AppendTag(rep.summary, "FF_TEMP_UNWRITTEN");
        }
        const bool stage0_writes_temp = (s.tss[0][gpu::kTssResultArg] == kTaTemp);
        for (uint32_t st = 0; st < 4u; ++st) {
            if ((rep.read_tex_mask & (1u << st)) && !stages[st].bound_va) {
                rep.flags |= kAnomFfUnboundTex;
                AppendTag(rep.summary, "FF_UNBOUND_TEX");
            }
            const uint32_t cop = s.tss[st][gpu::kTssColorOp];
            if (cop <= kTopDisable) break;
            const uint32_t a1 = s.tss[st][gpu::kTssColorArg1] & kTaMask;
            const uint32_t a2 = s.tss[st][gpu::kTssColorArg2] & kTaMask;
            if (cop == kTopSelectArg2 && a1 == kTaTexture && a2 != kTaTexture && stages[st].valid_desc) {
                rep.flags |= kAnomFfSelectDropsTex;
                AppendTag(rep.summary, "FF_SELARG2_DROPS_TEX");
            }
            // Only flag zero TFACTOR RGB on opaque non-TEMP-preserved stages (alpha-blended
            // shadows and TEMP-preserved optional night-glow stages intentionally use RGB=0).
            if (!blend_on && !(stage0_writes_temp && st > 0u) &&
                (cop == 4u || cop == 5u || cop == 6u) &&
                (a1 == kTaTfactor || a2 == kTaTfactor) &&
                (s.rs[gpu::kRsTextureFactor] & 0x00FFFFFFu) == 0u) {
                rep.flags |= kAnomFfTfactorBlack;
                AppendTag(rep.summary, "FF_TFACTOR_ZERO");
            }
        }
    }

    for (uint32_t st = 0; st < 4u; ++st) {
        // Only audit texture stages that the active pixel shader / FF combiner actually samples.
        if (!(rep.read_tex_mask & (1u << st))) continue;
        const StageTexInspection& t = stages[st];
        if (!t.bound_va) continue;

        if (t.p8_no_pal) {
            rep.flags |= kAnomP8NoPal;
            AppendTag(rep.summary, "P8_NO_PAL");
        }
        switch (t.sample_class) {
            case TexSampleClass::ZeroBlack:
            case TexSampleClass::AlphaZeroBlack:
                if (!rep.is_3d) {
                    // Full-screen linear video surfaces (Bink FMV intro fade-in) legitimately start black.
                    if (!(t.linear && t.width >= 512u && t.height >= 360u)) {
                        rep.flags |= kAnomUiBlackFrame;
                        AppendTag(rep.summary, "UI_BLACK_FRAME");
                    }
                } else {
                    rep.flags |= kAnomTexZeroBlack;
                    AppendTag(rep.summary, "TEX_ZERO_BLACK");
                }
                break;
            case TexSampleClass::SolidWhite:
                rep.flags |= kAnomTexSolidWhite;
                AppendTag(rep.summary, "TEX_SOLID_WHITE");
                break;
            case TexSampleClass::WhiteRgbAlphaMask:
                if (st == 0u && !blend_on && (!atest_on || eff_name != nullptr)) {
                    rep.flags |= kAnomWhiteMaskNoBlend;
                    AppendTag(rep.summary, "WHITE_MASK_NO_BLEND");
                }
                break;
            case TexSampleClass::SolidConst:
                // Valid in-RAM constant-color material swatches (e.g. 8x8 DXT1 / tinted DXT5) are benign.
                break;
            case TexSampleClass::InvalidDesc:
            case TexSampleClass::OutOfRam:
                rep.flags |= kAnomTexBadOrOor;
                AppendTag(rep.summary, "TEX_BAD_OR_OOR");
                break;
            case TexSampleClass::RenderTarget:
            case TexSampleClass::TransparentZero:
            default:
                break;
        }
    }

    if (!rep.vs_prog && rep.is_3d && (s.rs[gpu::kRsLighting] & 1u) != 0u && s.light_enable == 0u) {
        if ((s.tss[0][gpu::kTssColorArg1] & kTaMask) == kTaDiffuse ||
            (s.tss[0][gpu::kTssColorArg2] & kTaMask) == kTaDiffuse) {
            rep.flags |= kAnomLitNoLights;
            AppendTag(rep.summary, "LIT_NO_ACTIVE_LIGHTS");
        }
    }

    const bool st0_white_mask = (rep.read_tex_mask & 1u) != 0u &&
                                stages[0].sample_class == TexSampleClass::WhiteRgbAlphaMask;
    if (!s.ps && (eff_name != nullptr || st0_white_mask)) {
        const uint32_t cop0 = s.tss[0][gpu::kTssColorOp];
        const uint32_t ca1  = s.tss[0][gpu::kTssColorArg1] & kTaMask;
        const uint32_t ca2  = s.tss[0][gpu::kTssColorArg2] & kTaMask;
        if (eff_name != nullptr && st0_white_mask &&
            ((cop0 == kTopSelectArg1 && ca1 == kTaTexture) || (cop0 == kTopSelectArg2 && ca2 == kTaTexture))) {
            rep.flags |= kAnomWhiteEffectColorLost;
            AppendTag(rep.summary, "WHITE_EFF_UNMODULATED");
        } else if (eff_name != nullptr && cop0 >= kTopModulate &&
                   (ca1 == kTaDiffuse || ca2 == kTaDiffuse) && (rep.vs_regs_mask & (1u << 3)) == 0u) {
            rep.flags |= kAnomWhiteEffectColorLost;
            AppendTag(rep.summary, "WHITE_EFF_NO_VCOL");
        }
        // In FixedVertexHlsl, Lighting=1 without vertex normals (v2) drops vertex diffuse (v3)!
        if (!rep.vs_prog && rep.is_3d && (s.rs[gpu::kRsLighting] & 1u) != 0u &&
            (rep.vs_regs_mask & (1u << 3)) != 0u && (rep.vs_regs_mask & (1u << 2)) == 0u) {
            rep.flags |= kAnomWhiteEffectColorLost;
            AppendTag(rep.summary, "WHITE_EFF_LIT_DROPS_VCOL");
        }
    }

    if (eff_name != nullptr && vp.has_vcol && vp.all_white_rgb) {
        rep.flags |= kAnomEffectAllWhiteVcol;
        AppendTag(rep.summary, "EFF_ALL_WHITE_VCOL");
    }

    if (blend_on && dst_blend == 1u && (s.rs[gpu::kRsFogEnable] & 1u) != 0u && eff_name != nullptr) {
        rep.flags |= kAnomAdditiveWithFog;
        AppendTag(rep.summary, "ADDITIVE_WITH_FOG");
    }

    if (s.simple[gpu::kMAlphaTestEnable / 4u] != 0u) {
        const uint32_t func = s.simple[gpu::kMAlphaFunc / 4u] & 0xFFu;
        const uint32_t aref = s.simple[gpu::kMAlphaRef / 4u] & 0xFFu;
        if (func == 0x00u || func == 0x200u || (func == 0x204u && aref >= 255u)) {
            rep.flags |= kAnomAlphaKillAll;
            AppendTag(rep.summary, "ALPHA_TEST_KILL_ALL");
        }
    }

    if (rep.summary.empty()) rep.summary = "OK";
    return rep;
}

}  // namespace objlog
}  // namespace game
