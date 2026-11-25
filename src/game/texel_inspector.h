#pragma once
// Guest texture descriptor & texel payload classifier for render anomaly detection.

#include <cstdint>

#include "gpu/state.h"

namespace game {
namespace objlog {

enum class TexSampleClass : uint8_t {
    Unbound,
    RenderTarget,
    TransparentZero,
    InvalidDesc,
    OutOfRam,
    ZeroBlack,
    AlphaZeroBlack,
    SolidWhite,
    WhiteRgbAlphaMask,
    SolidConst,
    Varied,
    Unknown,
};

struct StageTexInspection {
    uint32_t bound_va = 0;
    uint32_t data_va = 0;
    uint32_t pal_va = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t levels = 0;
    uint32_t format = 0;
    const char* fmt_name = "NONE";
    bool valid_desc = false;
    bool cube = false;
    bool linear = false;
    bool is_p8 = false;
    bool rt_feedback = false;
    bool p8_no_pal = false;
    TexSampleClass sample_class = TexSampleClass::Unbound;
    uint32_t sample_dword = 0;
};

StageTexInspection InspectStageTexture(const gpu::State& s, uint32_t stage);
const char* SampleClassName(TexSampleClass c);

}  // namespace objlog
}  // namespace game
