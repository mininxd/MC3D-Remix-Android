#pragma once
// Thread-local effect context tracking, CPU vertex-diffuse inspection (immediate push,
// particle quad arrays, and bound vertex streams), and simple-RS desync detection.

#include <cstdint>
#include <string>

#include "gpu/state.h"

namespace game {
namespace objlog {

struct VertexProbeInfo {
    bool has_vcol = false;
    bool from_imm_push = false;
    bool from_particle_arr = false;
    bool from_vb0 = false;
    bool varied = false;
    bool all_white_rgb = false;
    bool all_alpha_zero = false;
    bool all_opaque = false;
    uint32_t effective_fvf = 0;  // Non-zero when dispatcher binds FVF inside the call (e.g. 0x144).
    uint32_t vert_count = 0;
    uint32_t first_argb = 0;
    uint32_t last_argb = 0;
};

// RAII thread-local tag identifying the active high-level MC3D effect emitter.
class ScopedEffectContext {
public:
    explicit ScopedEffectContext(const char* name);
    ~ScopedEffectContext();
    ScopedEffectContext(const ScopedEffectContext&) = delete;
    ScopedEffectContext& operator=(const ScopedEffectContext&) = delete;

private:
    const char* prev_ = nullptr;
};

const char* ActiveEffectName();

// RAII thread-local buffer holding sampled CPU vertex colors for the current draw.
class ScopedVertexProbe {
public:
    ScopedVertexProbe();
    ~ScopedVertexProbe();
    ScopedVertexProbe(const ScopedVertexProbe&) = delete;
    ScopedVertexProbe& operator=(const ScopedVertexProbe&) = delete;

private:
    VertexProbeInfo prev_{};
};

// Inspects MC3D's immediate push buffer before sub_002D5E40 commits vertices.
void ProbeImmediatePushColors();

// Inspects sub_002D6830's per-quad ARGB color array and records FVF 0x144 context.
void ProbeParticleColorArray(uint32_t color_arr_va, int32_t count);

// Returns current probe if set, or inspects s.streams[0] for D3DVSDT_D3DCOLOR (v3).
VertexProbeInfo ResolveVertexProbe(const gpu::State& s);

// Compares guest D3D_RenderState[57..91] against s.simple[method>>2] before MirrorHostStateToGuest.
void CapturePreSyncSimpleDesync(const gpu::State& s);
bool ConsumeSimpleRsDesync(std::string& out_detail);

// Formats blend, alpha test, fog, alpha combiner ops, vertex color, and effect tag for logs.
std::string FormatBlendAndEffectSuffix(const gpu::State& s, const VertexProbeInfo& vp, const char* eff_name);

}  // namespace objlog
}  // namespace game
