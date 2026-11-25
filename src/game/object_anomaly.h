#pragma once
// Analyzes bound Xbox D3D8/NV2A draw state to flag black/white/untextured anomalies.

#include <array>
#include <cstdint>
#include <string>

#include "game/texel_inspector.h"
#include "gpu/state.h"

namespace game {
namespace objlog {

enum AnomalyFlag : uint32_t {
    kAnomNone              = 0,
    kAnomProgUnboundTex    = 1u << 0,
    kAnomProgMode0Tex      = 1u << 1,
    kAnomFfUnboundTex      = 1u << 2,
    kAnomTexZeroBlack      = 1u << 3,
    kAnomTexSolidWhite     = 1u << 4,
    kAnomTexConst          = 1u << 5,
    kAnomTexBadOrOor       = 1u << 6,
    kAnomP8NoPal           = 1u << 7,
    kAnomFfTempUnwritten   = 1u << 8,
    kAnomLitNoLights       = 1u << 9,
    kAnomBadVsNoRegs       = 1u << 10,
    kAnomAlphaKillAll      = 1u << 11,
    kAnomFfTfactorBlack    = 1u << 12,
    kAnomFfSelectDropsTex  = 1u << 13,
    kAnomUiBlackFrame      = 1u << 14,
};

struct AnomalyReport {
    uint32_t flags = 0;
    uint32_t read_tex_mask = 0;
    uint16_t vs_regs_mask = 0;
    bool vs_prog = false;
    bool is_3d = true;
    std::string summary;
};

AnomalyReport AnalyzeDrawAnomalies(const gpu::State& s, const std::array<StageTexInspection, 4>& stages,
                                   uint32_t ps_read_mask);

}  // namespace objlog
}  // namespace game
