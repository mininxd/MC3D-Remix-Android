#pragma once
// Public exports for 3D model, texture load, particle/neon, audio, and per-object draw accuracy logging (object_logs.txt).

#include <cstdint>

#include "gpu/state.h"

namespace game {
namespace objlog {

void Init();
void SetDrawHint(const char* tag, uint32_t obj_va);
void ClearDrawHint();

void LogModelLoad(const char* kind, const char* name, uint32_t obj_va, uint32_t aux);
void LogResourcePageIn(const char* kind, uint32_t rsc_va, uint32_t arr_va, uint32_t count);
void LogDrawState(const gpu::State& s);
void OnSwapSummary();

// Audio telemetry hooks (called from src/sound/*, src/game/game.cpp, native/audio.cpp).
void LogAudioVoice(const char* event, uint32_t obj_va, bool is_stream, uint16_t tag, uint16_t channels,
                   uint32_t rate, uint16_t bits, bool is_3d, uint8_t nbins, const uint8_t* bins,
                   const int32_t* bin_mb, uint32_t max_packets, uint32_t callback_va);
void LogAudioPitch(uint32_t obj_va, bool is_stream, uint32_t base_rate, int32_t raw_pitch, uint32_t freq_hz);
void LogAudioIssue(bool is_error, const char* tag, uint32_t obj_va, const char* detail);
void RecordAudioPacketEvent(int kind);  // 0=submit, 1=complete, 2=flush, 3=underrun, 4=misaligned
void RecordAudioPumpEvent(bool from_bg_thread);
void RecordAudioRenderStats(uint32_t active_streams, uint32_t active_buffers, float reverb_peak);
void RecordAudioOutputStats(float peak_l, float peak_r, uint32_t clipped_samples, uint32_t nan_samples);

}  // namespace objlog
}  // namespace game
