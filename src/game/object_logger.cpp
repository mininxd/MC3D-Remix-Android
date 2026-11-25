#include "game/object_logger.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_set>

#include "game/guest_mem_safe.h"
#include "game/object_anomaly.h"
#include "game/object_logger_io.h"
#include "game/render_accuracy_internal.h"
#include "game/texel_inspector.h"
#include "gpu/texture.h"

namespace game {
namespace objlog {

namespace {

constexpr size_t kMaxLoggedLoads = 2048;
constexpr size_t kMaxLoggedDraws = 1536;

thread_local const char* tl_draw_tag = nullptr;
thread_local uint32_t tl_draw_obj_va = 0;

std::once_flag g_init_once;
std::unordered_set<uint64_t> g_logged_loads;
std::unordered_set<uint64_t> g_logged_draws;

std::atomic<uint64_t> g_total_draws{0};
std::atomic<uint64_t> g_anomalous_draws{0};
std::atomic<uint64_t> g_swap_frames{0};
std::array<std::atomic<uint64_t>, 12> g_anomaly_counts{};

uint64_t Mix64(uint64_t h, uint64_t v) {
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h;
}

uint64_t BuildDrawSignature(const gpu::State& s, const char* tag, uint32_t obj_va) {
    uint64_t h = 0xCBF29CE484222325ull;
    h = Mix64(h, obj_va);
    h = Mix64(h, tag ? uint64_t(std::uintptr_t(tag)) : 0ull);
    h = Mix64(h, s.vs_handle);
    h = Mix64(h, s.vs ? s.vs->hash : 0ull);
    h = Mix64(h, s.ps ? s.ps->handle : 0ull);
    h = Mix64(h, s.ps ? s.ps->def[54] : 0ull);
    h = Mix64(h, s.streams[0].vb);
    h = Mix64(h, s.streams[0].stride);
    h = Mix64(h, s.index_buffer);
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
    } else {
        h = Mix64(h, s.rs[gpu::kRsPsTextureModes]);
    }
    h = Mix64(h, (uint64_t(s.simple[gpu::kMBlendEnable / 4u]) << 8) |
                 (s.simple[gpu::kMAlphaTestEnable / 4u] & 1u));
    return h;
}

void RecordAnomalyCounters(uint32_t flags) {
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
        std::lock_guard<std::mutex> lk(detail::IoMutex());
        detail::EnsureOpenLocked();
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
                detail::WriteLineLocked(true, "[LOAD_TEX] name=\"%s\" gfx=0x%08X d3d=0x%08X %s %ux%u data=0x%08X",
                                        safe_name, obj_va, d3dtex, fi.name ? fi.name : "UNK",
                                        desc.width, desc.height, desc.data);
            } else {
                detail::WriteLineLocked(true, "[LOAD_TEX] name=\"%s\" gfx=0x%08X aux=0x%08X",
                                        safe_name, obj_va, aux);
            }
        }
        return;
    }

    detail::RegisterModelLocked(obj_va, safe_name);
    if (detail::InsertBoundedLocked(g_logged_loads, sig, kMaxLoggedLoads)) {
        detail::WriteLineLocked(true, "[LOAD_%s] name=\"%s\" obj=0x%08X aux=0x%08X",
                                kind ? kind : "MOD", safe_name, obj_va, aux);
    }
}

void LogResourcePageIn(const char* kind, uint32_t rsc_va, uint32_t arr_va, uint32_t count) {
    Init();
    std::lock_guard<std::mutex> lk(detail::IoMutex());
    const uint64_t sig = Mix64(Mix64(rsc_va, arr_va), count);
    if (!detail::InsertBoundedLocked(g_logged_loads, sig, kMaxLoggedLoads)) return;
    detail::WriteLineLocked(true, "[%s] rsc=0x%08X arr=0x%08X count=%u",
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
    const uint64_t sig = BuildDrawSignature(s, tag, obj_va);

    std::lock_guard<std::mutex> lk(detail::IoMutex());
    if (!detail::InsertBoundedLocked(g_logged_draws, sig, kMaxLoggedDraws)) return;

    std::array<StageTexInspection, 4> stages{};
    for (uint32_t i = 0; i < 4u; ++i) stages[i] = InspectStageTexture(s, i);

    const uint32_t ps_mask = s.ps ? game::render_accuracy::GetCombinerTexReadMask(s.ps->def) : 0u;
    const AnomalyReport anom = AnalyzeDrawAnomalies(s, stages, ps_mask);
    RecordAnomalyCounters(anom.flags);

    const std::string model_name = detail::LookupModelLocked(obj_va, s.streams[0].vb, s.index_buffer);
    const std::string tex_desc = FormatTexStages(stages, anom.read_tex_mask);
    const bool flush_now = (anom.flags != kAnomNone);

    if (s.ps) {
        detail::WriteLineLocked(
            flush_now,
            "[%s] tag=%s obj=0x%08X%s%s vs=0x%08X(%s,r=0x%X) vb0=0x%08X(s=%u) ib=0x%08X "
            "ps=0x%08X(modes=0x%05X,mask=0x%X) blend=%u/%u/%u alpha=%u%s ANOMALY=[%s]",
            anom.is_3d ? "OBJ3D" : "UI2D", tag, obj_va,
            model_name.empty() ? "" : " name=", model_name.empty() ? "" : model_name.c_str(),
            s.vs_handle, anom.vs_prog ? "prog" : "fvf", anom.vs_regs_mask,
            s.streams[0].vb, s.streams[0].stride, s.index_buffer,
            s.ps->handle, s.rs[gpu::kRsPsTextureModes], ps_mask,
            s.simple[gpu::kMBlendEnable / 4u] & 1u,
            s.simple[gpu::kMSrcBlend / 4u] & 0xFFu,
            s.simple[gpu::kMDestBlend / 4u] & 0xFFu,
            s.simple[gpu::kMAlphaTestEnable / 4u] & 1u,
            tex_desc.c_str(), anom.summary.c_str());
    } else {
        detail::WriteLineLocked(
            flush_now,
            "[%s] tag=%s obj=0x%08X%s%s vs=0x%08X(%s,r=0x%X) vb0=0x%08X(s=%u) ib=0x%08X "
            "FF cop=(%u,%u,%u,%u) arg1=(%u,%u,%u) res=(%u,%u) lit=%u(%u) blend=%u%s ANOMALY=[%s]",
            anom.is_3d ? "OBJ3D" : "UI2D", tag, obj_va,
            model_name.empty() ? "" : " name=", model_name.empty() ? "" : model_name.c_str(),
            s.vs_handle, anom.vs_prog ? "prog" : "fvf", anom.vs_regs_mask,
            s.streams[0].vb, s.streams[0].stride, s.index_buffer,
            s.tss[0][gpu::kTssColorOp], s.tss[1][gpu::kTssColorOp],
            s.tss[2][gpu::kTssColorOp], s.tss[3][gpu::kTssColorOp],
            s.tss[0][gpu::kTssColorArg1], s.tss[1][gpu::kTssColorArg1], s.tss[2][gpu::kTssColorArg1],
            s.tss[0][gpu::kTssResultArg], s.tss[1][gpu::kTssResultArg],
            s.rs[gpu::kRsLighting] & 1u, s.light_enable,
            s.simple[gpu::kMBlendEnable / 4u] & 1u,
            tex_desc.c_str(), anom.summary.c_str());
    }
}

void OnSwapSummary() {
    Init();
    const uint64_t f = ++g_swap_frames;
    if (f != 1u && (f % 180u) != 0u) return;

    std::lock_guard<std::mutex> lk(detail::IoMutex());
    detail::WriteLineLocked(
        true,
        "[SUMMARY] swap=%llu draws=%llu uniqueDraws=%zu badDraws=%llu modelsReg=%zu texReg=%zu "
        "counts=[progUnbound=%llu mode0=%llu ffUnbound=%llu texBlack=%llu texWhite=%llu p8NoPal=%llu tempUninit=%llu lit0=%llu]",
        static_cast<unsigned long long>(f),
        static_cast<unsigned long long>(g_total_draws.load()),
        g_logged_draws.size(),
        static_cast<unsigned long long>(g_anomalous_draws.load()),
        detail::RegisteredModelCountLocked(),
        detail::RegisteredTextureCountLocked(),
        static_cast<unsigned long long>(g_anomaly_counts[0].load()),
        static_cast<unsigned long long>(g_anomaly_counts[1].load()),
        static_cast<unsigned long long>(g_anomaly_counts[2].load()),
        static_cast<unsigned long long>(g_anomaly_counts[3].load()),
        static_cast<unsigned long long>(g_anomaly_counts[4].load()),
        static_cast<unsigned long long>(g_anomaly_counts[7].load()),
        static_cast<unsigned long long>(g_anomaly_counts[8].load()),
        static_cast<unsigned long long>(g_anomaly_counts[9].load()));
}

}  // namespace objlog
}  // namespace game
