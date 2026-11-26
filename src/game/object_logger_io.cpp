#include "game/object_logger_io.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <unordered_map>

#include "core/log.h"

namespace game {
namespace objlog {
namespace detail {

namespace {

constexpr size_t kMaxRegistryEntries = 8192;
constexpr uint32_t kFlushIntervalLines = 16;

std::mutex g_io_mu;
std::FILE* g_fp = nullptr;
bool g_open_attempted = false;
uint32_t g_pending_flush = 0;

std::unordered_map<uint32_t, std::string> g_model_names;
std::unordered_map<uint32_t, std::string> g_tex_names;
std::unordered_map<uint32_t, std::string> g_texdata_names;

void PutBounded(std::unordered_map<uint32_t, std::string>& map, uint32_t key, const char* name) {
    if (!key || !name || !name[0]) return;
    if (map.size() >= kMaxRegistryEntries && map.find(key) == map.end()) return;
    map[key] = name;
}

}  // namespace

std::mutex& IoMutex() { return g_io_mu; }

void EnsureOpenLocked() {
    if (g_fp || g_open_attempted) return;
    g_open_attempted = true;

    const char* candidates[] = {
        "/sdcard/mc3dr/object_logs.txt",
        "/storage/emulated/0/mc3dr/object_logs.txt",
        "object_logs.txt",
    };
    const char* chosen = nullptr;
    for (const char* path : candidates) {
        g_fp = std::fopen(path, "w");
        if (g_fp) {
            chosen = path;
            break;
        }
    }
    if (!g_fp) {
        LOGE("object_logs: failed to open object_logs.txt for writing");
        return;
    }

    std::fprintf(g_fp, "=== MC3D-Remix Android 3D Object, Particle, Neon & Audio Accuracy Log ===\n");
    std::fprintf(g_fp, "# Format: [SSSSS.MMM] [LEVEL ] [SECTION] key=value ...\n");
    std::fprintf(g_fp, "# Levels: [INFO  ] asset/summary  [OK    ] clean draw  [WARN  ] minor  [ERROR ] broken draw  [DANGER] critical\n");
    std::fprintf(g_fp, "# Sections: [LOAD_MOD] [LOAD_TEX] [PAGE_TEX] [PAGE_SHDR] [OBJ3D] [UI2D] [NEON] [PARTICLE] [AUDIO_VOICE] [AUDIO_PITCH] [AUDIO_WARN] [AUDIO_ERROR] [SUMMARY] [AUDIO_SUMMARY]\n");
    std::fprintf(g_fp, "# Filter examples: grep '\\[NEON' / grep '\\[PARTICLE' / grep '\\[AUDIO' / grep '\\[ERROR' / grep '\\[WARN'\n\n");
    std::fflush(g_fp);
    LOGR("[OK] object_logs: opened %s (structured INFO/OK/WARN/ERROR/DANGER format)", chosen);
}

void WriteLineVLocked(logfmt::Severity lv, bool flush_now, const char* fmt, va_list ap) {
    EnsureOpenLocked();
    if (!g_fp) return;
    char prefix[32];
    logfmt::FormatPrefix(lv, prefix, sizeof(prefix));
    std::fputs(prefix, g_fp);
    std::vfprintf(g_fp, fmt, ap);
    std::fputc('\n', g_fp);
    ++g_pending_flush;
    if (flush_now || g_pending_flush >= kFlushIntervalLines) {
        std::fflush(g_fp);
        g_pending_flush = 0;
    }
}

void WriteLineLocked(logfmt::Severity lv, bool flush_now, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    WriteLineVLocked(lv, flush_now, fmt, ap);
    va_end(ap);
}

void WriteFormatted(logfmt::Severity lv, bool flush_now, const char* fmt, ...) {
    std::lock_guard<std::mutex> lk(g_io_mu);
    va_list ap;
    va_start(ap, fmt);
    WriteLineVLocked(lv, flush_now, fmt, ap);
    va_end(ap);
}

void RegisterModelLocked(uint32_t obj_va, const char* name) {
    PutBounded(g_model_names, obj_va, name);
}

void RegisterTextureLocked(uint32_t gfx_tex_va, uint32_t d3d_tex_va, uint32_t data_va, const char* name) {
    PutBounded(g_model_names, gfx_tex_va, name);
    PutBounded(g_tex_names, d3d_tex_va, name);
    PutBounded(g_texdata_names, data_va, name);
}

std::string LookupModelLocked(uint32_t obj_va, uint32_t vb0_va, uint32_t ib_va) {
    if (obj_va) {
        auto it = g_model_names.find(obj_va);
        if (it != g_model_names.end()) return it->second;
    }
    if (vb0_va) {
        auto it = g_model_names.find(vb0_va);
        if (it != g_model_names.end()) return it->second;
    }
    if (ib_va) {
        auto it = g_model_names.find(ib_va);
        if (it != g_model_names.end()) return it->second;
    }
    return {};
}

std::string LookupTextureLocked(uint32_t d3d_tex_va, uint32_t data_va) {
    if (d3d_tex_va) {
        auto it = g_tex_names.find(d3d_tex_va);
        if (it != g_tex_names.end()) return it->second;
    }
    if (data_va) {
        auto it = g_texdata_names.find(data_va);
        if (it != g_texdata_names.end()) return it->second;
    }
    return {};
}

bool InsertBoundedLocked(std::unordered_set<uint64_t>& set, uint64_t key, size_t cap) {
    if (set.find(key) != set.end()) return false;
    if (set.size() >= cap) return false;
    set.insert(key);
    return true;
}

size_t RegisteredModelCountLocked() { return g_model_names.size(); }
size_t RegisteredTextureCountLocked() { return g_tex_names.size() + g_texdata_names.size(); }

}  // namespace detail
}  // namespace objlog
}  // namespace game
