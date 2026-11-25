#include "game/object_anomaly.h"

#include "gpu/vertex.h"

namespace game {
namespace objlog {

namespace {

constexpr uint32_t kTopDisable     = 1u;
constexpr uint32_t kTopSelectArg1  = 2u;
constexpr uint32_t kTopSelectArg2  = 3u;
constexpr uint32_t kTaMask         = 0x0Fu;
constexpr uint32_t kTaDiffuse      = 0u;
constexpr uint32_t kTaTexture      = 2u;
constexpr uint32_t kTaTfactor      = 3u;
constexpr uint32_t kTaTemp         = 5u;

void AppendTag(std::string& dst, const char* tag) {
    if (!dst.empty()) dst.push_back('|');
    dst += tag;
}

uint16_t CollectInputRegMask(const gpu::State& s, bool& out_vs_prog, bool& out_xyzrhw) {
    out_vs_prog = (s.vs_handle & 1u) != 0 && s.vs && !s.vs->program.empty();
    out_xyzrhw = !(s.vs_handle & 1u) && ((s.vs_handle & 0x0Eu) == 0x04u);

    const gpu::VertexShader* active_vs = s.vs.get();
    gpu::VertexShader fvf_vs{};
    if (!active_vs && s.vs_handle != 0 && !(s.vs_handle & 1u)) {
        gpu::FvfInputs(s.vs_handle, fvf_vs);
        active_vs = &fvf_vs;
    }

    uint16_t mask = 0;
    if (active_vs) {
        for (int r = 0; r < 16; ++r) {
            if (active_vs->inputs[r].stream >= 0) mask |= static_cast<uint16_t>(1u << r);
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
                                   uint32_t ps_read_mask) {
    AnomalyReport rep{};
    bool xyzrhw = false;
    rep.vs_regs_mask = CollectInputRegMask(s, rep.vs_prog, xyzrhw);
    rep.is_3d = !xyzrhw;

    if ((s.vs_handle != 0 || s.vs != nullptr) && rep.vs_regs_mask == 0) {
        rep.flags |= kAnomBadVsNoRegs;
        AppendTag(rep.summary, "BAD_VS_NO_REGS");
    }

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
            if ((cop == 4u || cop == 5u || cop == 6u) &&
                (a1 == kTaTfactor || a2 == kTaTfactor) &&
                (s.rs[gpu::kRsTextureFactor] & 0x00FFFFFFu) == 0u) {
                rep.flags |= kAnomFfTfactorBlack;
                AppendTag(rep.summary, "FF_TFACTOR_ZERO");
            }
        }
    }

    for (uint32_t st = 0; st < 4u; ++st) {
        if (!(rep.read_tex_mask & (1u << st)) && !stages[st].bound_va) continue;
        const StageTexInspection& t = stages[st];
        if (!t.bound_va) continue;

        if (t.p8_no_pal) {
            rep.flags |= kAnomP8NoPal;
            AppendTag(rep.summary, "P8_NO_PAL");
        }
        switch (t.sample_class) {
            case TexSampleClass::ZeroBlack:
            case TexSampleClass::AlphaZeroBlack:
                rep.flags |= kAnomTexZeroBlack;
                AppendTag(rep.summary, "TEX_ZERO_BLACK");
                break;
            case TexSampleClass::SolidWhite:
                rep.flags |= kAnomTexSolidWhite;
                AppendTag(rep.summary, "TEX_SOLID_WHITE");
                break;
            case TexSampleClass::SolidConst:
                rep.flags |= kAnomTexConst;
                AppendTag(rep.summary, "TEX_SOLID_CONST");
                break;
            case TexSampleClass::InvalidDesc:
            case TexSampleClass::OutOfRam:
                rep.flags |= kAnomTexBadOrOor;
                AppendTag(rep.summary, "TEX_BAD_OR_OOR");
                break;
            default:
                break;
        }
    }

    if (!rep.vs_prog && rep.is_3d && s.rs[gpu::kRsLighting] != 0u && s.light_enable == 0u) {
        if ((s.tss[0][gpu::kTssColorArg1] & kTaMask) == kTaDiffuse ||
            (s.tss[0][gpu::kTssColorArg2] & kTaMask) == kTaDiffuse) {
            rep.flags |= kAnomLitNoLights;
            AppendTag(rep.summary, "LIT_NO_ACTIVE_LIGHTS");
        }
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
