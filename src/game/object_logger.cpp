#include "game/object_logger.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_set>

#include "core/log.h"
#include "game/effect_probe.h"
#include "game/guest_mem_safe.h"
#include "game/log_severity.h"
#include "game/object_anomaly.h"
#include "game/object_logger_io.h"
#include "game/render_accuracy_internal.h"
#include "game/rt_tracker.h"
#include "game/texel_inspector.h"
#include "game/vk_format_probe.h"
#include "gpu/texture.h"

namespace game {
namespace objlog {

namespace {

constexpr size_t kMaxLoggedLoads = 1024;
constexpr size_t kMaxLoggedDraws = 1536;
constexpr size_t kMaxLoggedEffects = 384;
constexpr size_t kMaxLoggedAnomalies = 512;
constexpr size_t kMaxLoggedAudio = 384;
constexpr size_t kMaxTlFastSeen = 2048;

thread_local const char* tl_draw_tag = nullptr;
thread_local uint32_t tl_draw_obj_va = 0;
thread_local std::unordered_set<uint64_t> tl_fast_seen;

std::once_flag g_init_once;
std::unordered_set<uint64_t> g_logged_loads;
std::unordered_set<uint64_t> g_logged_draws;
std::unordered_set<uint64_t> g_logged_effects;
std::unordered_set<uint64_t> g_logged_anomalies;
std::unordered_set<uint64_t> g_logged_audio;

std::atomic<uint64_t> g_total_draws{0};
std::atomic<uint64_t> g_anomalous_draws{0};
std::atomic<uint64_t> g_ok_draws{0};
std::atomic<uint64_t> g_warn_draws{0};
std::atomic<uint64_t> g_error_draws{0};
std::atomic<uint64_t> g_danger_draws{0};
std::atomic<uint64_t> g_swap_frames{0};
std::atomic<uint64_t> g_neon_draws{0};
std::atomic<uint64_t> g_particle_draws{0};
std::atomic<uint64_t> g_car_light_draws{0};
std::atomic<uint64_t> g_light_quad_draws{0};
std::array<std::atomic<uint64_t>, 24> g_anomaly_counts{};
uint64_t g_prev_ok = 0;
uint64_t g_prev_warn = 0;
uint64_t g_prev_error = 0;
uint64_t g_prev_danger = 0;

// Audio telemetry counters (lock-free hot path from AAudio/DSound render thread).
std::atomic<uint64_t> g_audio_streams_created{0};
std::atomic<uint64_t> g_audio_buffers_created{0};
std::atomic<uint64_t> g_audio_pkts_submitted{0};
std::atomic<uint64_t> g_audio_pkts_completed{0};
std::atomic<uint64_t> g_audio_pkts_flushed{0};
std::atomic<uint64_t> g_audio_stream_underruns{0};
std::atomic<uint64_t> g_audio_pkts_misaligned{0};
std::atomic<uint64_t> g_audio_dowork_pumps{0};
std::atomic<uint64_t> g_audio_bg_pumps{0};
std::atomic<uint64_t> g_audio_mix_blocks{0};
std::atomic<uint64_t> g_audio_clipped_samples{0};
std::atomic<uint64_t> g_audio_nan_samples{0};
std::atomic<uint32_t> g_audio_active_streams{0};
std::atomic<uint32_t> g_audio_active_buffers{0};
std::atomic<uint32_t> g_audio_peak_l_milli{0};
std::atomic<uint32_t> g_audio_peak_r_milli{0};
std::atomic<uint32_t> g_audio_reverb_peak_milli{0};
std::atomic<uint32_t> g_audio_pitch_logs{0};
std::atomic<uint32_t> g_audio_issue_logs{0};
uint64_t g_prev_audio_underruns = 0;
uint64_t g_prev_audio_clips = 0;

void UpdateAtomicMaxU32(std::atomic<uint32_t>& dst, uint32_t val) {
    uint32_t cur = dst.load(std::memory_order_relaxed);
    while (val > cur && !dst.compare_exchange_weak(cur, val, std::memory_order_relaxed)) {}
}

const char* AudioFormatName(uint16_t tag) {
    switch (tag) {
        case 1u: return "PCM";
        case 0x69u: return "XBOX_ADPCM";
        case 0x161u: return "WMA";
        default: return "UNKNOWN";
    }
}

const char* SectionTagForDraw(bool is_3d, const char* eff_name) {
    if (eff_name) {
        if (std::strcmp(eff_name, "NEON_GLOW") == 0) return "NEON";
        if (std::strcmp(eff_name, "PARTICLE_FX") == 0 ||
            std::strcmp(eff_name, "PARTICLE_BILLBOARD") == 0 ||
            std::strcmp(eff_name, "CAR_LIGHT_FX") == 0 ||
            std::strcmp(eff_name, "LIGHT_QUAD_FX") == 0) {
            return "PARTICLE";
        }
    }
    return is_3d ? "OBJ3D" : "UI2D";
}

uint64_t Mix64(uint64_t h, uint64_t v) {
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h;
}

uint64_t BuildFastDrawSignature(const gpu::State& s, const char* tag, const char* eff_name) {
    uint64_t h = 0xCBF29CE484222325ull;
    h = Mix64(h, tag ? uint64_t(std::uintptr_t(tag)) : 0ull);
    h = Mix64(h, eff_name ? uint64_t(std::uintptr_t(eff_name)) : 0ull);
    h = Mix64(h, s.vs_handle);
    h = Mix64(h, s.vs ? s.vs->hash : 0ull);
    h = Mix64(h, s.ps ? s.ps->handle : 0ull);
    h = Mix64(h, s.ps ? s.ps->def[54] : 0ull);
    for (uint32_t i = 0; i < 4u; ++i) {
        h = Mix64(h, (uint64_t(s.textures[i]) << 32) | s.palettes[i]);
    }
    if (!s.ps) {
        for (uint32_t i = 0; i < 4u; ++i) {
            h = Mix64(h, (uint64_t(s.tss[i][gpu::kTssColorOp]) << 24) |
                         (uint64_t(s.tss[i][gpu::kTssColorArg1]) << 16) |
                         (uint64_t(s.tss[i][gpu::kTssColorArg2]) << 8) |
                         s.tss[i][gpu::kTssResultArg]);
        }
        h = Mix64(h, (uint64_t(s.rs[gpu::kRsLighting]) << 16) | s.light_enable);
        h = Mix64(h, s.rs[gpu::kRsFogEnable] & 1u);
    } else {
        h = Mix64(h, s.rs[gpu::kRsPsTextureModes]);
    }
    h = Mix64(h, (uint64_t(s.simple[gpu::kMBlendEnable / 4u] & 1u) << 32) |
                 (uint64_t(s.simple[gpu::kMSrcBlend / 4u] & 0xFFFFu) << 16) |
                 (s.simple[gpu::kMDestBlend / 4u] & 0xFFFEu) |
                 (s.simple[gpu::kMAlphaTestEnable / 4u] & 1u));
    h = Mix64(h, s.simple[gpu::kMBlendOp / 4u] & 0xFFFFu);
    return h;
}

uint64_t FinalizeDrawSignature(uint64_t fast_sig, const gpu::State& s, const VertexProbeInfo& vp) {
    uint64_t h = fast_sig;
    if (!s.vs_handle && vp.effective_fvf) h = Mix64(h, vp.effective_fvf);
    if (vp.has_vcol) {
        // Bucket vertex color by semantic class rather than raw fade ticks so fading particles
        // and taillights do not saturate the log cap or bloat object_logs.txt.
        const uint64_t vclass = vp.all_white_rgb ? 1ull : (((vp.first_argb >> 24) == 0u) ? 2ull : 3ull);
        h = Mix64(h, vclass);
    }
    return h;
}

void RecordAnomalyCounters(uint32_t flags, logfmt::Severity lv) {
    switch (lv) {
        case logfmt::Severity::Ok:     ++g_ok_draws; break;
        case logfmt::Severity::Warn:   ++g_warn_draws; break;
        case logfmt::Severity::Error:  ++g_error_draws; break;
        case logfmt::Severity::Danger: ++g_danger_draws; break;
        case logfmt::Severity::Info:   break;
    }
    if (flags == kAnomNone) return;
    ++g_anomalous_draws;
    for (uint32_t bit = 0; bit < g_anomaly_counts.size(); ++bit) {
        if (flags & (1u << bit)) ++g_anomaly_counts[bit];
    }
}

std::string FormatTexStages(const std::array<StageTexInspection, 4>& st, uint32_t read_mask) {
    std::string out;
    for (uint32_t i = 0; i < 4u; ++i) {
        if (!st[i].bound_va && !(read_mask & (1u << i))) continue;
        char buf[224];
        std::string tname = st[i].bound_va ? detail::LookupTextureLocked(st[i].bound_va, st[i].data_va) : "";
        if (!st[i].bound_va) {
            std::snprintf(buf, sizeof(buf), " t%u=UNBOUND", i);
        } else {
            std::snprintf(buf, sizeof(buf),
                          " t%u=[%s %ux%u va=0x%08X d=0x%08X pal=0x%X px=%s%s%s]",
                          i, st[i].fmt_name, st[i].width, st[i].height,
                          st[i].bound_va, st[i].data_va, st[i].pal_va,
                          SampleClassName(st[i].sample_class),
                          tname.empty() ? "" : " name=",
                          tname.empty() ? "" : tname.c_str());
        }
        out += buf;
    }
    return out;
}

}  // namespace

void Init() {
    std::call_once(g_init_once, [] {
        const std::string vk_line = FormatVkProbeLine();
        const logfmt::Severity vk_lv = IsBgra8VertexSupported() ? logfmt::Severity::Info : logfmt::Severity::Danger;
        std::lock_guard<std::mutex> lk(detail::IoMutex());
        detail::EnsureOpenLocked();
        detail::WriteLineLocked(vk_lv, true, "%s", vk_line.c_str());
        LOGR("[%s] %s", IsBgra8VertexSupported() ? "INFO" : "DANGER", vk_line.c_str());
    });
}

void SetDrawHint(const char* tag, uint32_t obj_va) {
    tl_draw_tag = tag;
    tl_draw_obj_va = obj_va;
}

void ClearDrawHint() {
    tl_draw_tag = nullptr;
    tl_draw_obj_va = 0;
}

void LogModelLoad(const char* kind, const char* name, uint32_t obj_va, uint32_t aux) {
    Init();
    const char* safe_name = (name && name[0]) ? name : "<unnamed>";
    uint64_t sig = Mix64(obj_va, aux);
    for (const char* p = safe_name; *p; ++p) sig = Mix64(sig, static_cast<uint8_t>(*p));

    std::lock_guard<std::mutex> lk(detail::IoMutex());
    if (std::strcmp(kind, "TEX") == 0) {
        gpu::TextureDesc desc{};
        uint32_t d3dtex = 0;
        if (aux && safe_mem::CanAccess(aux, 20u) && gpu::Describe(aux, desc)) d3dtex = aux;
        detail::RegisterTextureLocked(obj_va, d3dtex, desc.data, safe_name);
        if (detail::InsertBoundedLocked(g_logged_loads, sig, kMaxLoggedLoads)) {
            if (d3dtex) {
                const gpu::FormatInfo& fi = gpu::Format(desc.format);
                detail::WriteLineLocked(logfmt::Severity::Info, true,
                                        "[LOAD_TEX] name=\"%s\" gfx=0x%08X d3d=0x%08X %s %ux%u data=0x%08X",
                                        safe_name, obj_va, d3dtex, fi.name ? fi.name : "UNK",
                                        desc.width, desc.height, desc.data);
            } else {
                detail::WriteLineLocked(logfmt::Severity::Info, true,
                                        "[LOAD_TEX] name=\"%s\" gfx=0x%08X aux=0x%08X",
                                        safe_name, obj_va, aux);
            }
        }
        return;
    }

    detail::RegisterModelLocked(obj_va, safe_name);
    if (detail::InsertBoundedLocked(g_logged_loads, sig, kMaxLoggedLoads)) {
        detail::WriteLineLocked(logfmt::Severity::Info, true,
                                "[LOAD_%s] name=\"%s\" obj=0x%08X aux=0x%08X",
                                kind ? kind : "MOD", safe_name, obj_va, aux);
    }
}

void LogResourcePageIn(const char* kind, uint32_t rsc_va, uint32_t arr_va, uint32_t count) {
    Init();
    std::lock_guard<std::mutex> lk(detail::IoMutex());
    const uint64_t sig = Mix64(Mix64(rsc_va, arr_va), count);
    if (!detail::InsertBoundedLocked(g_logged_loads, sig, kMaxLoggedLoads)) return;
    detail::WriteLineLocked(logfmt::Severity::Info, true, "[%s] rsc=0x%08X arr=0x%08X count=%u",
                            kind ? kind : "PAGE", rsc_va, arr_va, count);

    if (std::strcmp(kind, "PAGE_TEX") == 0 && arr_va && count > 0) {
        const uint32_t lim = std::min<uint32_t>(count, 64u);
        for (uint32_t i = 0; i < lim; ++i) {
            const uint32_t tex_obj = safe_mem::Read32(arr_va + i * 4u, 0);
            if (!tex_obj) continue;
            const uint32_t candidate_d3d = safe_mem::Read32(tex_obj + 0x08u, 0);
            gpu::TextureDesc desc{};
            if (candidate_d3d && safe_mem::CanAccess(candidate_d3d, 20u) && gpu::Describe(candidate_d3d, desc)) {
                char tag_name[48];
                std::snprintf(tag_name, sizeof(tag_name), "paged_tex_%08X", tex_obj);
                detail::RegisterTextureLocked(tex_obj, candidate_d3d, desc.data, tag_name);
            }
        }
    }
}

void LogDrawState(const gpu::State& s) {
    Init();
    ++g_total_draws;

    const char* tag = tl_draw_tag ? tl_draw_tag : "APPLY";
    const uint32_t obj_va = tl_draw_obj_va;
    const char* eff_name = ActiveEffectName();
    if (eff_name) {
        if (std::strcmp(eff_name, "NEON_GLOW") == 0) ++g_neon_draws;
        else if (std::strcmp(eff_name, "CAR_LIGHT_FX") == 0) ++g_car_light_draws;
        else if (std::strcmp(eff_name, "LIGHT_QUAD_FX") == 0) ++g_light_quad_draws;
        else ++g_particle_draws;
    }

    static std::atomic<bool> s_normal_budget_full{false};
    static std::atomic<bool> s_effect_budget_full{false};
    static std::atomic<bool> s_anom_budget_full{false};
    if (s_normal_budget_full.load(std::memory_order_relaxed) &&
        s_anom_budget_full.load(std::memory_order_relaxed) &&
        (eff_name == nullptr || s_effect_budget_full.load(std::memory_order_relaxed))) {
        return;
    }

    const uint64_t fast_sig = BuildFastDrawSignature(s, tag, eff_name);

    // Lock-free fast path: skip vertex-buffer probing and IoMutex on already-logged draw states.
    if (tl_fast_seen.find(fast_sig) != tl_fast_seen.end()) return;

    const VertexProbeInfo vp = ResolveVertexProbe(s);
    const uint64_t sig = FinalizeDrawSignature(fast_sig, s, vp);

    std::lock_guard<std::mutex> lk(detail::IoMutex());
    if (tl_fast_seen.size() < kMaxTlFastSeen) tl_fast_seen.insert(fast_sig);

    const bool under_cap = !s_normal_budget_full.load(std::memory_order_relaxed) &&
                           (g_logged_draws.size() < kMaxLoggedDraws);
    bool is_new_effect = false;
    if (eff_name != nullptr) {
        if (g_logged_effects.find(sig) != g_logged_effects.end()) return;
        is_new_effect = !s_effect_budget_full.load(std::memory_order_relaxed) &&
                        detail::InsertBoundedLocked(g_logged_effects, sig, kMaxLoggedEffects);
        if (!is_new_effect && !under_cap && s_anom_budget_full.load(std::memory_order_relaxed)) return;
    }

    const bool already_logged_normal = under_cap
        ? !detail::InsertBoundedLocked(g_logged_draws, sig, kMaxLoggedDraws)
        : (g_logged_draws.find(sig) != g_logged_draws.end() ||
           g_logged_anomalies.find(sig) != g_logged_anomalies.end());
    if (already_logged_normal && !is_new_effect) return;

    std::array<StageTexInspection, 4> stages{};
    for (uint32_t i = 0; i < 4u; ++i) stages[i] = InspectStageTexture(s, i);

    const uint32_t ps_mask = s.ps ? game::render_accuracy::GetCombinerTexReadMask(s.ps->def) : 0u;
    const AnomalyReport anom = AnalyzeDrawAnomalies(s, stages, ps_mask, vp, eff_name);
    const logfmt::Severity sev = logfmt::ClassifyAnomalyFlags(anom.flags);
    RecordAnomalyCounters(anom.flags, sev);
    if (!under_cap && !is_new_effect) {
        if (anom.flags == kAnomNone ||
            s_anom_budget_full.load(std::memory_order_relaxed) ||
            !detail::InsertBoundedLocked(g_logged_anomalies, sig, kMaxLoggedAnomalies)) {
            return;
        }
    }

    static uint32_t s_normal_lines = 0;
    static uint32_t s_effect_lines = 0;
    static uint32_t s_anom_lines = 0;
    constexpr uint32_t kMaxDetailLogLines = 1536u;
    constexpr uint32_t kMaxEffectLogLines = 384u;
    constexpr uint32_t kMaxAnomalyLogLines = 256u;
    if (is_new_effect) {
        if (++s_effect_lines >= kMaxEffectLogLines) {
            s_effect_budget_full.store(true, std::memory_order_relaxed);
        }
    } else if (!under_cap && anom.flags != kAnomNone) {
        if (++s_anom_lines >= kMaxAnomalyLogLines) {
            s_anom_budget_full.store(true, std::memory_order_relaxed);
        }
    } else {
        if (++s_normal_lines >= kMaxDetailLogLines) {
            s_normal_budget_full.store(true, std::memory_order_relaxed);
        }
    }

    const char* section = SectionTagForDraw(anom.is_3d, eff_name);
    const std::string model_name = detail::LookupModelLocked(obj_va, s.streams[0].vb, s.index_buffer);
    const std::string tex_desc = FormatTexStages(stages, anom.read_tex_mask);
    const std::string blend_eff = FormatBlendAndEffectSuffix(s, vp, eff_name);
    const bool flush_now = is_new_effect ||
                           (s_normal_lines <= 64u && (sev == logfmt::Severity::Error || sev == logfmt::Severity::Danger));

    if (s.ps) {
        detail::WriteLineLocked(
            sev, flush_now,
            "[%s] tag=%s obj=0x%08X%s%s vs=0x%08X(%s,r=0x%X) vb0=0x%08X(s=%u) ib=0x%08X "
            "ps=0x%08X(modes=0x%05X,mask=0x%X)%s%s ANOMALY=[%s]",
            section, tag, obj_va,
            model_name.empty() ? "" : " name=", model_name.empty() ? "" : model_name.c_str(),
            anom.eff_vs_handle, anom.vs_prog ? "prog" : "fvf", anom.vs_regs_mask,
            s.streams[0].vb, s.streams[0].stride, s.index_buffer,
            s.ps->handle, s.rs[gpu::kRsPsTextureModes], ps_mask,
            blend_eff.c_str(), tex_desc.c_str(), anom.summary.c_str());
    } else {
        detail::WriteLineLocked(
            sev, flush_now,
            "[%s] tag=%s obj=0x%08X%s%s vs=0x%08X(%s,r=0x%X) vb0=0x%08X(s=%u) ib=0x%08X "
            "FF cop=(%u,%u,%u,%u) a1=(%u,%u,%u) a2=(%u,%u,%u) aop=(%u,%u,%u) res=(%u,%u) tfa=0x%08X lit=%u(%u)%s%s ANOMALY=[%s]",
            section, tag, obj_va,
            model_name.empty() ? "" : " name=", model_name.empty() ? "" : model_name.c_str(),
            anom.eff_vs_handle, anom.vs_prog ? "prog" : "fvf", anom.vs_regs_mask,
            s.streams[0].vb, s.streams[0].stride, s.index_buffer,
            s.tss[0][gpu::kTssColorOp], s.tss[1][gpu::kTssColorOp],
            s.tss[2][gpu::kTssColorOp], s.tss[3][gpu::kTssColorOp],
            s.tss[0][gpu::kTssColorArg1], s.tss[1][gpu::kTssColorArg1], s.tss[2][gpu::kTssColorArg1],
            s.tss[0][gpu::kTssColorArg2], s.tss[1][gpu::kTssColorArg2], s.tss[2][gpu::kTssColorArg2],
            s.tss[0][gpu::kTssAlphaOp], s.tss[1][gpu::kTssAlphaOp], s.tss[2][gpu::kTssAlphaOp],
            s.tss[0][gpu::kTssResultArg], s.tss[1][gpu::kTssResultArg],
            s.rs[gpu::kRsTextureFactor],
            s.rs[gpu::kRsLighting] & 1u, s.light_enable,
            blend_eff.c_str(), tex_desc.c_str(), anom.summary.c_str());
    }
}

void LogAudioVoice(const char* event, uint32_t obj_va, bool is_stream, uint16_t tag, uint16_t channels,
                   uint32_t rate, uint16_t bits, bool is_3d, uint8_t nbins, const uint8_t* bins,
                   const int32_t* bin_mb, uint32_t max_packets, uint32_t callback_va) {
    Init();
    if (event && std::strcmp(event, "CREATE") == 0) {
        if (is_stream) ++g_audio_streams_created;
        else ++g_audio_buffers_created;
    }

    uint64_t sig = Mix64(is_stream ? 1ull : 0ull, tag);
    sig = Mix64(sig, (uint64_t(channels) << 32) | rate);
    sig = Mix64(sig, (uint64_t(bits) << 16) | (is_3d ? 1u : 0u) | (uint64_t(nbins) << 8));
    if (event) {
        for (const char* p = event; *p; ++p) sig = Mix64(sig, static_cast<uint8_t>(*p));
    }
    if (bins && nbins > 0) {
        for (uint8_t i = 0; i < nbins && i < 8u; ++i) sig = Mix64(sig, bins[i]);
    }
    // Always log unique streams by obj_va up to cap so radio/music/SFX streams are individually traceable.
    if (is_stream) sig = Mix64(sig, obj_va);

    std::lock_guard<std::mutex> lk(detail::IoMutex());
    if (!detail::InsertBoundedLocked(g_logged_audio, sig, kMaxLoggedAudio)) return;

    std::string bin_str;
    if (bins && nbins > 0) {
        char b[32];
        for (uint8_t i = 0; i < nbins && i < 8u; ++i) {
            const uint8_t id = bins[i] & 31u;
            const int32_t mb = bin_mb ? bin_mb[id] : 0;
            std::snprintf(b, sizeof(b), "%s%u:%d", i ? "," : "", id, mb);
            bin_str += b;
        }
    } else {
        bin_str = "default(FL,FR)";
    }

    const bool bad_fmt = (tag != 1u && tag != 0x69u);
    const logfmt::Severity lv = bad_fmt ? logfmt::Severity::Error : logfmt::Severity::Info;
    detail::WriteLineLocked(
        lv, bad_fmt || is_stream,
        "[AUDIO_VOICE] ev=%s kind=%s obj=0x%08X fmt=%s(0x%X) ch=%u rate=%u bits=%u 3d=%u maxPkt=%u cb=0x%08X bins=[%s]",
        event ? event : "EVT", is_stream ? "STREAM" : "BUF", obj_va,
        AudioFormatName(tag), tag, channels, rate, bits, is_3d ? 1u : 0u,
        max_packets, callback_va, bin_str.c_str());
}

void LogAudioPitch(uint32_t obj_va, bool is_stream, uint32_t base_rate, int32_t raw_pitch, uint32_t freq_hz) {
    Init();
    const uint32_t n = g_audio_pitch_logs.fetch_add(1u, std::memory_order_relaxed);
    if (n >= 24u) return;
    std::lock_guard<std::mutex> lk(detail::IoMutex());
    detail::WriteLineLocked(
        logfmt::Severity::Info, false,
        "[AUDIO_PITCH] kind=%s obj=0x%08X baseRate=%u rawPitch=%d -> freqHz=%u",
        is_stream ? "STREAM" : "BUF", obj_va, base_rate, raw_pitch, freq_hz);
}

void LogAudioIssue(bool is_error, const char* tag, uint32_t obj_va, const char* detail_msg) {
    Init();
    const uint32_t n = g_audio_issue_logs.fetch_add(1u, std::memory_order_relaxed);
    if (n >= 64u) return;
    std::lock_guard<std::mutex> lk(detail::IoMutex());
    detail::WriteLineLocked(
        is_error ? logfmt::Severity::Error : logfmt::Severity::Warn, true,
        "[%s] tag=%s obj=0x%08X %s",
        is_error ? "AUDIO_ERROR" : "AUDIO_WARN",
        tag ? tag : "AUDIO", obj_va, detail_msg ? detail_msg : "");
}

void RecordAudioPacketEvent(int kind) {
    switch (kind) {
        case 0: ++g_audio_pkts_submitted; break;
        case 1: ++g_audio_pkts_completed; break;
        case 2: ++g_audio_pkts_flushed; break;
        case 3: ++g_audio_stream_underruns; break;
        case 4: ++g_audio_pkts_misaligned; break;
        default: break;
    }
}

void RecordAudioPumpEvent(bool from_bg_thread) {
    if (from_bg_thread) ++g_audio_bg_pumps;
    else ++g_audio_dowork_pumps;
}

void RecordAudioRenderStats(uint32_t active_streams, uint32_t active_buffers, float reverb_peak) {
    g_audio_active_streams.store(active_streams, std::memory_order_relaxed);
    g_audio_active_buffers.store(active_buffers, std::memory_order_relaxed);
    UpdateAtomicMaxU32(g_audio_reverb_peak_milli, static_cast<uint32_t>(std::min(reverb_peak, 99.0f) * 1000.0f));
}

void RecordAudioOutputStats(float peak_l, float peak_r, uint32_t clipped_samples, uint32_t nan_samples) {
    ++g_audio_mix_blocks;
    if (clipped_samples) g_audio_clipped_samples.fetch_add(clipped_samples, std::memory_order_relaxed);
    if (nan_samples) g_audio_nan_samples.fetch_add(nan_samples, std::memory_order_relaxed);
    UpdateAtomicMaxU32(g_audio_peak_l_milli, static_cast<uint32_t>(std::min(peak_l, 99.0f) * 1000.0f));
    UpdateAtomicMaxU32(g_audio_peak_r_milli, static_cast<uint32_t>(std::min(peak_r, 99.0f) * 1000.0f));
}

void OnSwapSummary() {
    Init();
    const uint64_t f = ++g_swap_frames;
    if (f != 1u && (f % 180u) != 0u) return;

    std::lock_guard<std::mutex> lk(detail::IoMutex());
    const uint64_t ok_n = g_ok_draws.load();
    const uint64_t warn_n = g_warn_draws.load();
    const uint64_t error_n = g_error_draws.load();
    const uint64_t danger_n = g_danger_draws.load();
    const uint64_t d_ok = ok_n - g_prev_ok;
    const uint64_t d_warn = warn_n - g_prev_warn;
    const uint64_t d_err = error_n - g_prev_error;
    const uint64_t d_dng = danger_n - g_prev_danger;
    g_prev_ok = ok_n;
    g_prev_warn = warn_n;
    g_prev_error = error_n;
    g_prev_danger = danger_n;

    // Reflect current window health in [SUMMARY] severity so early intro fades do not
    // pin every later gameplay summary to [ERROR].
    const logfmt::Severity sum_lv =
        (d_dng > 0u) ? logfmt::Severity::Danger :
        (d_err > 0u) ? logfmt::Severity::Error  : logfmt::Severity::Info;
    detail::WriteLineLocked(
        sum_lv, true,
        "[SUMMARY] swap=%llu draws=%llu ok=%llu(+%llu) warn=%llu(+%llu) error=%llu(+%llu) danger=%llu(+%llu) "
        "uniqueDraws=%zu uniqueEff=%zu uniqueAnom=%zu badDraws=%llu rtTracked=%u modelsReg=%zu texReg=%zu "
        "effDraws=[neon=%llu ptcl=%llu carLt=%llu ltQuad=%llu] "
        "counts=[progUnbound=%llu mode0=%llu ffUnbound=%llu texBlack=%llu texWhite=%llu p8NoPal=%llu "
        "tempUninit=%llu lit0=%llu tf0=%llu sel2Drop=%llu uiBlack=%llu wMaskNoB=%llu wEffLost=%llu "
        "addFog=%llu rsDesync=%llu vkBgra=%llu effWhtV=%llu]",
        static_cast<unsigned long long>(f),
        static_cast<unsigned long long>(g_total_draws.load()),
        static_cast<unsigned long long>(ok_n),
        static_cast<unsigned long long>(d_ok),
        static_cast<unsigned long long>(warn_n),
        static_cast<unsigned long long>(d_warn),
        static_cast<unsigned long long>(error_n),
        static_cast<unsigned long long>(d_err),
        static_cast<unsigned long long>(danger_n),
        static_cast<unsigned long long>(d_dng),
        g_logged_draws.size(),
        g_logged_effects.size(),
        g_logged_anomalies.size(),
        static_cast<unsigned long long>(g_anomalous_draws.load()),
        rt_tracker::TrackedTargetCount(),
        detail::RegisteredModelCountLocked(),
        detail::RegisteredTextureCountLocked(),
        static_cast<unsigned long long>(g_neon_draws.load()),
        static_cast<unsigned long long>(g_particle_draws.load()),
        static_cast<unsigned long long>(g_car_light_draws.load()),
        static_cast<unsigned long long>(g_light_quad_draws.load()),
        static_cast<unsigned long long>(g_anomaly_counts[0].load()),
        static_cast<unsigned long long>(g_anomaly_counts[1].load()),
        static_cast<unsigned long long>(g_anomaly_counts[2].load()),
        static_cast<unsigned long long>(g_anomaly_counts[3].load()),
        static_cast<unsigned long long>(g_anomaly_counts[4].load()),
        static_cast<unsigned long long>(g_anomaly_counts[7].load()),
        static_cast<unsigned long long>(g_anomaly_counts[8].load()),
        static_cast<unsigned long long>(g_anomaly_counts[9].load()),
        static_cast<unsigned long long>(g_anomaly_counts[12].load()),
        static_cast<unsigned long long>(g_anomaly_counts[13].load()),
        static_cast<unsigned long long>(g_anomaly_counts[14].load()),
        static_cast<unsigned long long>(g_anomaly_counts[15].load()),
        static_cast<unsigned long long>(g_anomaly_counts[16].load()),
        static_cast<unsigned long long>(g_anomaly_counts[17].load()),
        static_cast<unsigned long long>(g_anomaly_counts[18].load()),
        static_cast<unsigned long long>(g_anomaly_counts[19].load()),
        static_cast<unsigned long long>(g_anomaly_counts[20].load()));

    const uint64_t underruns = g_audio_stream_underruns.load(std::memory_order_relaxed);
    const uint64_t clips = g_audio_clipped_samples.load(std::memory_order_relaxed);
    const uint64_t nans = g_audio_nan_samples.load(std::memory_order_relaxed);
    const uint64_t d_underruns = underruns - g_prev_audio_underruns;
    const uint64_t d_clips = clips - g_prev_audio_clips;
    g_prev_audio_underruns = underruns;
    g_prev_audio_clips = clips;
    const float peak_l = g_audio_peak_l_milli.exchange(0u, std::memory_order_relaxed) / 1000.0f;
    const float peak_r = g_audio_peak_r_milli.exchange(0u, std::memory_order_relaxed) / 1000.0f;
    const float rev_pk = g_audio_reverb_peak_milli.exchange(0u, std::memory_order_relaxed) / 1000.0f;
    const logfmt::Severity audio_lv =
        (nans > 0u) ? logfmt::Severity::Error :
        (d_underruns > 0u) ? logfmt::Severity::Warn : logfmt::Severity::Info;
    detail::WriteLineLocked(
        audio_lv, true,
        "[AUDIO_SUMMARY] swap=%llu active=(str=%u,buf=%u) created=(str=%llu,buf=%llu) "
        "pkts=(sub=%llu,done=%llu,flush=%llu,misalign=%llu,underrun=%llu(+%llu)) "
        "pumps=(doWork=%llu,bg=%llu) mix=(blocks=%llu,peakL=%.2f,peakR=%.2f,revPeak=%.2f,limited=%llu(+%llu),nan=%llu)",
        static_cast<unsigned long long>(f),
        g_audio_active_streams.load(std::memory_order_relaxed),
        g_audio_active_buffers.load(std::memory_order_relaxed),
        static_cast<unsigned long long>(g_audio_streams_created.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_audio_buffers_created.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_audio_pkts_submitted.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_audio_pkts_completed.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_audio_pkts_flushed.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_audio_pkts_misaligned.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(underruns),
        static_cast<unsigned long long>(d_underruns),
        static_cast<unsigned long long>(g_audio_dowork_pumps.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_audio_bg_pumps.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_audio_mix_blocks.load(std::memory_order_relaxed)),
        peak_l, peak_r, rev_pk,
        static_cast<unsigned long long>(clips),
        static_cast<unsigned long long>(d_clips),
        static_cast<unsigned long long>(nans));
}

}  // namespace objlog
}  // namespace game
