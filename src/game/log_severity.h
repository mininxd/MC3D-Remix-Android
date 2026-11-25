#pragma once
// Structured severity classification & timestamp formatting for log-reader compatibility
// (INFO, OK, WARN, ERROR, DANGER) across object_logs.txt and app_logs.txt.

#include <cstddef>
#include <cstdint>

namespace game {
namespace logfmt {

enum class Severity : uint8_t {
    Info,
    Ok,
    Warn,
    Error,
    Danger,
};

// Fixed-width uppercase severity label for aligned log-reader parsing & filtering.
const char* SeverityTag(Severity lv);

// Classifies an object_anomaly bitmask into OK / WARN / ERROR / DANGER.
Severity ClassifyAnomalyFlags(uint32_t anomaly_flags);

// Elapsed seconds since first log call (monotonic clock, formatted as [SSSSS.MMM]).
double ElapsedSeconds();

// Formats "[SSSSS.MMM] [LEVEL ] " into out_buf and returns characters written.
int FormatPrefix(Severity lv, char* out_buf, size_t buf_size);

}  // namespace logfmt
}  // namespace game
