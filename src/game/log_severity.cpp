#include "game/log_severity.h"

#include <chrono>
#include <cstdio>

#include "game/object_anomaly.h"

namespace game {
namespace logfmt {

namespace {

const auto g_start_time = std::chrono::steady_clock::now();

constexpr uint32_t kDangerMask =
    objlog::kAnomBadVsNoRegs |
    objlog::kAnomTexBadOrOor |
    objlog::kAnomAlphaKillAll |
    objlog::kAnomVkBgraVertexUnsupported;

constexpr uint32_t kErrorMask =
    objlog::kAnomProgUnboundTex |
    objlog::kAnomProgMode0Tex |
    objlog::kAnomFfUnboundTex |
    objlog::kAnomTexZeroBlack |
    objlog::kAnomP8NoPal |
    objlog::kAnomFfTempUnwritten |
    objlog::kAnomFfSelectDropsTex |
    objlog::kAnomWhiteMaskNoBlend |
    objlog::kAnomWhiteEffectColorLost |
    objlog::kAnomSimpleRsDesync;

}  // namespace

const char* SeverityTag(Severity lv) {
    switch (lv) {
        case Severity::Info:   return "INFO  ";
        case Severity::Ok:     return "OK    ";
        case Severity::Warn:   return "WARN  ";
        case Severity::Error:  return "ERROR ";
        case Severity::Danger: return "DANGER";
    }
    return "INFO  ";
}

Severity ClassifyAnomalyFlags(uint32_t flags) {
    if (flags == objlog::kAnomNone) return Severity::Ok;
    if ((flags & kDangerMask) != 0u) return Severity::Danger;
    if ((flags & kErrorMask) != 0u) return Severity::Error;
    return Severity::Warn;
}

double ElapsedSeconds() {
    const auto now = std::chrono::steady_clock::now();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_start_time).count();
    return static_cast<double>(ms) / 1000.0;
}

int FormatPrefix(Severity lv, char* out_buf, size_t buf_size) {
    if (!out_buf || buf_size == 0u) return 0;
    return std::snprintf(out_buf, buf_size, "[%9.3f] [%s] ", ElapsedSeconds(), SeverityTag(lv));
}

}  // namespace logfmt
}  // namespace game
