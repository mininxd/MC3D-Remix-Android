// The Android host: SDL's window and event loop, the settings and folders, the app's lifecycle; the game is described
// by app::kGame (src/common/game_info.cpp)
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <android/log.h>
#include <sys/system_properties.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <cwchar>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#include <toml++/toml.hpp>

#include "app/app.h"
#include "build_info.h"
#include "common/options.h"
#include "core/input.h"
#include "core/log.h"
#include "crash_log.h"
#include "host.h"
#include "image_symbols.h"
#include "kernel/kernel.h"

namespace {

SDL_Window* g_window = nullptr;
std::atomic<int> g_win_w{0};
std::atomic<int> g_win_h{0};
std::atomic<bool> g_foreground{true};
std::atomic<uint32_t> g_surface_epoch{0};
std::mutex g_foreground_lock;
std::condition_variable g_foreground_cv;

std::string Utf8(const wchar_t* w) {
    std::string s;
    for (; w && *w; w++) s += *w < 0x80 ? char(*w) : '?';
    return s;
}

// the log's lines to logcat as well: stderr is read on a thread of its own
void LogToLogcat() {
    // handled directly by logx::Write using __android_log_print with tag MC3DR
}

std::string Property(const char* name) {
    char v[PROP_VALUE_MAX] = {};
    __system_property_get(name, v);
    return v;
}

void SetForeground(bool on) {
    {
        std::lock_guard lock(g_foreground_lock);
        if (on && !g_foreground) g_surface_epoch++;
        g_foreground = on;
    }
    g_foreground_cv.notify_all();
    LOGI("app: %s", on ? "in front" : "in the background");
}

// SDL's display mode in landscape: the window is not up yet when the settings load
app::Mode ScreenMode() {
    if (g_window) {
        int w = 0, h = 0;
        host::WindowPixels(w, h);
        if (w > 0 && h > 0) return {uint32_t(w), uint32_t(h)};
    }
    const SDL_DisplayMode* m = SDL_GetCurrentDisplayMode(SDL_GetPrimaryDisplay());
    if (!m) return {1920, 1080};
    uint32_t w = uint32_t(m->w * m->pixel_density), h = uint32_t(m->h * m->pixel_density);
    return {std::max(w, h), std::min(w, h)};
}

float RefreshRate() {
    SDL_DisplayID d = g_window ? SDL_GetDisplayForWindow(g_window) : SDL_GetPrimaryDisplay();
    const SDL_DisplayMode* m = SDL_GetCurrentDisplayMode(d);
    return m && m->refresh_rate > 1 ? m->refresh_rate : 60.0f;
}

}  // namespace

namespace host {

SDL_Window* Window() { return g_window; }
bool Foreground() { return g_foreground; }
uint32_t SurfaceEpoch() { return g_surface_epoch; }

void WaitForeground() {
    std::unique_lock lock(g_foreground_lock);
    g_foreground_cv.wait(lock, [] { return g_foreground.load(); });
}

void RefreshWindowPixels() {
    if (!g_window) return;
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(g_window, &w, &h);
    if (w > 0 && h > 0) {
        g_win_w.store(w, std::memory_order_relaxed);
        g_win_h.store(h, std::memory_order_relaxed);
    }
}

void WindowPixels(int& w, int& h) {
    w = g_win_w.load(std::memory_order_relaxed);
    h = g_win_h.load(std::memory_order_relaxed);
    if ((w <= 0 || h <= 0) && g_window) {
        SDL_GetWindowSizeInPixels(g_window, &w, &h);
        if (w > 0 && h > 0) {
            g_win_w.store(w, std::memory_order_relaxed);
            g_win_h.store(h, std::memory_order_relaxed);
        }
    }
}

namespace {

std::mutex g_diag_lock;
std::string g_diag_device;
std::string g_diag_cpu;
std::string g_diag_gpu = "Vulkan GPU";
std::string g_diag_vk_api = "Vulkan 1.1";
std::string g_diag_android = "Android";
uint32_t g_diag_ram_total_mb = 0;
bool g_diag_hw_init = false;

std::atomic<float> g_diag_fps{0.0f};
std::atomic<float> g_diag_p1_low{0.0f};
std::atomic<float> g_diag_frame_ms{16.67f};
std::atomic<float> g_diag_cpu_pct{0.0f};
std::atomic<float> g_diag_gpu_pct{0.0f};
std::atomic<float> g_diag_temp_c{-1.0f};
std::atomic<uint32_t> g_diag_draws{0};
std::atomic<uint64_t> g_diag_verts{0};
std::atomic<uint32_t> g_diag_culled{0};
std::atomic<uint32_t> g_diag_ram_used_mb{0};
std::atomic<uint32_t> g_diag_tex_count{0};
std::atomic<uint32_t> g_diag_obj_hit_pct{0};
std::atomic<uint32_t> g_diag_render_w{0};
std::atomic<uint32_t> g_diag_render_h{0};

// 1% Low FPS ring buffer (last 120 frame times, sorted every 500ms in Debug mode)
constexpr size_t kFrameTimeHist = 120;
float g_ft_ring[kFrameTimeHist]{};
size_t g_ft_pos = 0;
size_t g_ft_count = 0;

// CPU & GPU sysfs sampling state (NFSU2 DiagnosticOverlay style, sampled every 500ms)
uint64_t g_prev_cpu_total = 0;
uint64_t g_prev_cpu_work = 0;
int64_t g_prev_wall_ms = 0;
int64_t g_prev_proc_ms = 0;
float g_last_cpu_pct = 0.0f;
float g_smoothed_gpu_pct = -1.0f;
bool g_gpu_sysfs_checked = false;
std::string g_gpu_sysfs_path;
bool g_gpu_sysfs_pair = false;
bool g_temp_sysfs_checked = false;
std::string g_temp_sysfs_path;
std::chrono::steady_clock::time_point g_last_diag_sample{};

std::string TrimStr(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    return s;
}

std::string TitleBrand(std::string s) {
    s = TrimStr(std::move(s));
    if (s.empty()) return s;
    bool all_lower = true;
    for (unsigned char c : s) {
        if (std::isupper(c)) {
            all_lower = false;
            break;
        }
    }
    if (all_lower) s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
    return s;
}

void EnsureHardwareInfoLocked() {
    if (g_diag_hw_init) return;
    g_diag_hw_init = true;

    std::string brand = TitleBrand(Property("ro.product.brand"));
    if (brand.empty()) brand = TitleBrand(Property("ro.product.manufacturer"));
    std::string model = TrimStr(Property("ro.product.model"));
    if (!brand.empty() && !model.empty()) {
        std::string b_low = brand, m_low = model;
        for (char& c : b_low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        for (char& c : m_low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        g_diag_device = (m_low.rfind(b_low, 0) == 0) ? model : (brand + " " + model);
    } else if (!model.empty()) {
        g_diag_device = model;
    } else if (!brand.empty()) {
        g_diag_device = brand;
    } else {
        g_diag_device = "Android Device";
    }

    std::string soc_mfg = TitleBrand(Property("ro.soc.manufacturer"));
    std::string soc_model = TrimStr(Property("ro.soc.model"));
    if (!soc_model.empty()) {
        std::string sm_low = soc_model, mfg_low = soc_mfg;
        for (char& c : sm_low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        for (char& c : mfg_low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (!soc_mfg.empty() && sm_low.rfind(mfg_low, 0) != 0) {
            g_diag_cpu = soc_mfg + " " + soc_model;
        } else {
            g_diag_cpu = soc_model;
        }
    } else {
        std::string hw = TrimStr(Property("ro.board.platform"));
        if (hw.empty()) hw = TrimStr(Property("ro.hardware"));
        if (hw.empty()) {
            std::ifstream ci("/proc/cpuinfo");
            for (std::string line; std::getline(ci, line);) {
                if (line.rfind("Hardware", 0) == 0) {
                    const size_t col = line.find(':');
                    if (col != std::string::npos) {
                        hw = TrimStr(line.substr(col + 1));
                        break;
                    }
                }
            }
        }
        g_diag_cpu = hw.empty() ? "ARM64 CPU" : hw;
    }

    std::string rel = TrimStr(Property("ro.build.version.release"));
    g_diag_android = rel.empty() ? "Android" : ("Android " + rel);
    g_diag_ram_total_mb = uint32_t(std::max(0, SDL_GetSystemRAM()));
}

uint32_t SampleProcessRamMb() {
    std::ifstream f("/proc/self/status");
    for (std::string line; std::getline(f, line);) {
        if (line.rfind("VmRSS:", 0) == 0) {
            unsigned long long kb = 0;
            if (std::sscanf(line.c_str() + 6, "%llu", &kb) == 1) {
                return uint32_t(kb / 1024ull);
            }
            break;
        }
    }
    return 0u;
}

bool ReadTempFileC(const std::string& path, float& out_c) {
    std::ifstream f(path);
    std::string line;
    if (!std::getline(f, line)) return false;
    float raw = 0.0f;
    if (std::sscanf(line.c_str(), "%f", &raw) != 1 || raw <= 0.0f) return false;
    float c = raw;
    if (raw > 1000.0f) c = raw / 1000.0f;
    else if (raw > 120.0f) c = raw / 10.0f;
    if (c >= 10.0f && c <= 110.0f) {
        out_c = c;
        return true;
    }
    return false;
}

float SampleTempC() {
    float c = -1.0f;
    if (g_temp_sysfs_checked) {
        if (g_temp_sysfs_path.empty()) return -1.0f;
        if (ReadTempFileC(g_temp_sysfs_path, c)) return c;
        g_temp_sysfs_path.clear();
        return -1.0f;
    }
    g_temp_sysfs_checked = true;
    static constexpr const char* kTempPaths[] = {
        "/sys/class/power_supply/battery/temp",
        "/sys/class/power_supply/bms/temp",
        "/sys/class/thermal/thermal_zone0/temp",
        "/sys/class/thermal/thermal_zone1/temp",
    };
    for (const char* p : kTempPaths) {
        if (ReadTempFileC(p, c)) {
            g_temp_sysfs_path = p;
            return c;
        }
    }
    return -1.0f;
}

float SampleCpuPercent() {
    {
        std::ifstream f("/proc/stat");
        std::string line;
        if (std::getline(f, line)) {
            uint64_t user = 0, nice = 0, sys = 0, idle = 0, iowait = 0, irq = 0, softirq = 0;
            if (std::sscanf(line.c_str(), "cpu %llu %llu %llu %llu %llu %llu %llu",
                            (unsigned long long*)&user, (unsigned long long*)&nice,
                            (unsigned long long*)&sys, (unsigned long long*)&idle,
                            (unsigned long long*)&iowait, (unsigned long long*)&irq,
                            (unsigned long long*)&softirq) >= 7) {
                const uint64_t total = user + nice + sys + idle + iowait + irq + softirq;
                const uint64_t work = user + nice + sys;
                if (g_prev_cpu_total > 0 && total > g_prev_cpu_total && work >= g_prev_cpu_work) {
                    const uint64_t td = total - g_prev_cpu_total;
                    const uint64_t wd = work - g_prev_cpu_work;
                    g_last_cpu_pct = std::clamp(float(wd) * 100.0f / float(td), 0.0f, 100.0f);
                }
                g_prev_cpu_total = total;
                g_prev_cpu_work = work;
                return g_last_cpu_pct;
            }
        }
    }
    // Fallback: process CPU time normalized by logical cores (identical to NFSU2 DiagnosticOverlay)
    struct timespec ts_wall{}, ts_proc{};
    clock_gettime(CLOCK_MONOTONIC, &ts_wall);
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts_proc);
    const int64_t now_wall = int64_t(ts_wall.tv_sec) * 1000 + ts_wall.tv_nsec / 1000000;
    const int64_t now_proc = int64_t(ts_proc.tv_sec) * 1000 + ts_proc.tv_nsec / 1000000;
    if (g_prev_wall_ms > 0 && now_wall > g_prev_wall_ms && now_proc >= g_prev_proc_ms) {
        const int cores = std::max(1, SDL_GetNumLogicalCPUCores());
        const float pct = float(now_proc - g_prev_proc_ms) * 100.0f / (float(now_wall - g_prev_wall_ms) * float(cores));
        g_last_cpu_pct = std::clamp(pct, 0.0f, 100.0f);
    }
    g_prev_wall_ms = now_wall;
    g_prev_proc_ms = now_proc;
    return g_last_cpu_pct;
}

bool ReadSysfsPercent(const std::string& path, float& out) {
    std::ifstream f(path);
    std::string line;
    if (!std::getline(f, line)) return false;
    float v = -1.0f;
    if (std::sscanf(line.c_str(), "%f", &v) == 1 && v >= 0.0f && v <= 100.0f) {
        out = v;
        return true;
    }
    return false;
}

bool ReadSysfsBusyPair(const std::string& path, float& out) {
    std::ifstream f(path);
    std::string line;
    if (!std::getline(f, line)) return false;
    unsigned long long busy = 0, total = 0;
    if (std::sscanf(line.c_str(), "%llu %llu", &busy, &total) == 2 && total > 0 && busy <= total) {
        out = float(busy) * 100.0f / float(total);
        return true;
    }
    return false;
}

bool SampleGpuSysfs(float& out) {
    if (g_gpu_sysfs_checked) {
        if (g_gpu_sysfs_path.empty()) return false;
        const bool ok = g_gpu_sysfs_pair ? ReadSysfsBusyPair(g_gpu_sysfs_path, out)
                                         : ReadSysfsPercent(g_gpu_sysfs_path, out);
        if (!ok) g_gpu_sysfs_path.clear();
        return ok;
    }
    g_gpu_sysfs_checked = true;
    static constexpr const char* kPctPaths[] = {
        "/sys/class/kgsl/kgsl-3d0/gpu_busy_percentage",
        "/sys/class/kgsl/kgsl-3d0/gpu_load",
        "/sys/class/misc/mali0/device/utilization",
        "/sys/class/misc/mali0/device/gpu_busy",
        "/sys/devices/platform/mali.0/utilization",
        "/sys/devices/platform/13040000.mali/utilization",
        "/sys/devices/platform/13000000.mali/utilization",
        "/sys/devices/platform/14ac0000.mali/utilization",
        "/sys/devices/platform/1c500000.mali/utilization",
        "/sys/devices/platform/sprd_mali.0/utilization",
        "/sys/devices/platform/gpu/utilization",
        "/sys/kernel/debug/ged/hal/gpu_utilization",
        "/sys/module/ged/parameters/gpu_loading",
        "/sys/module/ged/parameters/gpu_dvfs_loading",
    };
    for (const char* p : kPctPaths) {
        if (ReadSysfsPercent(p, out)) {
            g_gpu_sysfs_path = p;
            g_gpu_sysfs_pair = false;
            return true;
        }
    }
    static constexpr const char* kBusyPaths[] = {
        "/sys/class/kgsl/kgsl-3d0/gpubusy",
        "/sys/devices/platform/soc/soc:qcom,kgsl-3d0/kgsl/kgsl-3d0/gpubusy",
    };
    for (const char* p : kBusyPaths) {
        if (ReadSysfsBusyPair(p, out)) {
            g_gpu_sysfs_path = p;
            g_gpu_sysfs_pair = true;
            return true;
        }
    }
    return false;
}

}  // namespace

void SetGpuInfo(const char* name, uint32_t vk_major, uint32_t vk_minor) {
    std::lock_guard lock(g_diag_lock);
    if (name && name[0]) g_diag_gpu = name;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "Vulkan %u.%u", vk_major, vk_minor);
    g_diag_vk_api = buf;
}

void UpdateDisplayLogs(const DisplayLogsFrame& frame) {
    g_diag_fps.store(frame.fps, std::memory_order_relaxed);
    g_diag_frame_ms.store(frame.frame_ms, std::memory_order_relaxed);
    g_diag_draws.store(frame.draws, std::memory_order_relaxed);
    g_diag_verts.store(frame.vertices, std::memory_order_relaxed);
    g_diag_culled.store(frame.culled, std::memory_order_relaxed);
    g_diag_tex_count.store(frame.tex_count, std::memory_order_relaxed);
    g_diag_obj_hit_pct.store(frame.obj_hit_pct, std::memory_order_relaxed);
    g_diag_render_w.store(frame.render_w, std::memory_order_relaxed);
    g_diag_render_h.store(frame.render_h, std::memory_order_relaxed);

    const uint32_t mode = game::Opt().display_logs;
    if (mode == 0u) return;

    if (mode >= 3u && frame.raw_frame_ms > 0.2f && frame.raw_frame_ms < 250.0f) {
        g_ft_ring[g_ft_pos] = frame.raw_frame_ms;
        g_ft_pos = (g_ft_pos + 1u) % kFrameTimeHist;
        if (g_ft_count < kFrameTimeHist) ++g_ft_count;
    }

    const auto now = std::chrono::steady_clock::now();
    if (g_last_diag_sample.time_since_epoch().count() != 0 &&
        std::chrono::duration_cast<std::chrono::milliseconds>(now - g_last_diag_sample).count() < 500) {
        return;
    }
    g_last_diag_sample = now;

    const float cpu = SampleCpuPercent();
    g_diag_cpu_pct.store(cpu, std::memory_order_relaxed);

    float gpu = 0.0f;
    if (SampleGpuSysfs(gpu)) {
        g_smoothed_gpu_pct = gpu;
    } else {
        // Balanced fallback estimator (NFSU2 DiagnosticOverlay style + Vulkan GPU wait ratio)
        const float target_fps = float(app::Get().fps_cap >= 30u ? app::Get().fps_cap : 60u);
        const float target_ms = 1000.0f / target_fps;
        float est = 0.0f;
        if (frame.gpu_wait_ms > 0.5f && frame.frame_ms > 1.0f) {
            est = std::clamp((frame.gpu_wait_ms / target_ms) * 100.0f, 15.0f, 99.0f);
        } else if (frame.fps >= target_fps * 0.90f) {
            est = std::clamp(25.0f + cpu * 0.20f, 20.0f, 45.0f);
        } else if (cpu > 70.0f) {
            const float ratio = std::max(0.2f, frame.fps / target_fps);
            est = std::clamp(ratio * 45.0f, 18.0f, 50.0f);
        } else {
            const float eff = std::min(1.0f, target_ms / std::max(1.0f, frame.frame_ms));
            est = std::clamp(eff * 60.0f, 25.0f, 65.0f);
        }
        g_smoothed_gpu_pct = (g_smoothed_gpu_pct < 0.0f) ? est : (g_smoothed_gpu_pct * 0.75f + est * 0.25f);
        gpu = std::clamp(g_smoothed_gpu_pct, 0.0f, 100.0f);
    }
    g_diag_gpu_pct.store(gpu, std::memory_order_relaxed);

    if (mode >= 3u) {
        if (g_ft_count >= 10u) {
            float sorted[kFrameTimeHist];
            std::memcpy(sorted, g_ft_ring, g_ft_count * sizeof(float));
            std::sort(sorted, sorted + g_ft_count);
            const size_t p99_idx = std::min(g_ft_count - 1u, (g_ft_count * 99u) / 100u);
            const float p99_ms = std::max(sorted[p99_idx], 1.0f);
            g_diag_p1_low.store(std::min(frame.fps > 0.1f ? frame.fps : 999.0f, 1000.0f / p99_ms),
                                std::memory_order_relaxed);
        } else {
            g_diag_p1_low.store(frame.fps, std::memory_order_relaxed);
        }
        g_diag_ram_used_mb.store(SampleProcessRamMb(), std::memory_order_relaxed);
        g_diag_temp_c.store(SampleTempC(), std::memory_order_relaxed);
    }
}

DisplayLogsSnapshot GetDisplayLogs() {
    DisplayLogsSnapshot snap{};
    {
        std::lock_guard lock(g_diag_lock);
        EnsureHardwareInfoLocked();
        snap.device_name = g_diag_device.c_str();
        snap.cpu_name = g_diag_cpu.c_str();
        snap.gpu_name = g_diag_gpu.c_str();
        snap.vulkan_api = g_diag_vk_api.c_str();
        snap.android_ver = g_diag_android.c_str();
        snap.ram_total_mb = g_diag_ram_total_mb;
    }
    snap.app_version = APP_VERSION_NAME;
    snap.build_commit = MC3D_COMMIT;
    snap.fps = g_diag_fps.load(std::memory_order_relaxed);
    snap.p1_low_fps = g_diag_p1_low.load(std::memory_order_relaxed);
    snap.frame_ms = g_diag_frame_ms.load(std::memory_order_relaxed);
    snap.cpu_percent = g_diag_cpu_pct.load(std::memory_order_relaxed);
    snap.gpu_percent = g_diag_gpu_pct.load(std::memory_order_relaxed);
    snap.temp_c = g_diag_temp_c.load(std::memory_order_relaxed);
    snap.draws = g_diag_draws.load(std::memory_order_relaxed);
    snap.vertices = g_diag_verts.load(std::memory_order_relaxed);
    snap.culled = g_diag_culled.load(std::memory_order_relaxed);
    snap.ram_used_mb = g_diag_ram_used_mb.load(std::memory_order_relaxed);
    snap.tex_count = g_diag_tex_count.load(std::memory_order_relaxed);
    snap.obj_hit_pct = g_diag_obj_hit_pct.load(std::memory_order_relaxed);
    snap.render_pct = dynres::Percent();
    snap.render_w = g_diag_render_w.load(std::memory_order_relaxed);
    snap.render_h = g_diag_render_h.load(std::memory_order_relaxed);
    return snap;
}

}  // namespace host

namespace app {

namespace {

const char* kModes[] = {"fullscreen", "borderless", "windowed"};
// the defaults a settings file was written under: 3 enables dynamic resolution and sets 70% render scale once.
constexpr int kDefaults = 3;

std::filesystem::path File() { return ExeDir() / kGame.settings; }

toml::table Table() {
    const Settings& s = Get();
    toml::table t{{"display_mode", kModes[uint32_t(s.display_mode)]},
                  {"width", int64_t(s.width)},
                  {"height", int64_t(s.height)},
                  {"render_percent", int64_t(s.render_percent)},
                  {"fps_cap", int64_t(s.fps_cap)},
                  {"vsync", s.vsync},
                  {"anisotropic_filtering", int64_t(s.aniso)},
                  {"join_ip", s.join_ip},
                  {"defaults", int64_t(kDefaults)}};
    for (const Option& o : kGame.options) {
        if (o.flag) t.insert(o.key, *o.flag);
        else t.insert(o.key, int64_t(*o.value));
    }
    if (kGame.save) kGame.save(&t);
    return t;
}

// a phone renders at 75% default render scale unless settings or dynamic resolution override it
constexpr uint32_t kFirstRunRenderPercent = 75;

void PhoneFrameDefaults(Settings& s) {
    s.render_percent = kFirstRunRenderPercent;
    s.fps_cap = 60;
    s.vsync = false;
    game::Options& o = game::Opt();
    o.dynamic_resolution = true;
    o.dynamic_min_percent = 30;
    o.dynamic_max_percent = 90;
    o.dynamic_target_fps = s.fps_cap;
}

}  // namespace

// the settings as a phone starts with them, for a first run and the options' restore
Settings DefaultSettings() {
    Settings s;
    s.render_percent = kFirstRunRenderPercent;
    PhoneFrameDefaults(s);
    return s;
}

Settings& Get() {
    static Settings s = DefaultSettings();
    return s;
}

void Load() {
    Settings& s = Get();
    toml::table t;
    try {
        t = toml::parse_file(File().string());
    } catch (...) {
        return;
    }
    std::string mode = t["display_mode"].value_or(std::string(kModes[uint32_t(s.display_mode)]));
    for (uint32_t i = 0; i < 3; i++)
        if (mode == kModes[i]) s.display_mode = DisplayMode(i);
    s.width = t["width"].value_or(s.width);
    s.height = t["height"].value_or(s.height);
    s.render_percent = t["render_percent"].value_or(s.render_percent);
    s.fps_cap = t["fps_cap"].value_or(s.fps_cap);
    s.vsync = t["vsync"].value_or(s.vsync);
    s.aniso = t["anisotropic_filtering"].value_or(s.aniso);
    s.join_ip = t["join_ip"].value_or(s.join_ip);
    const bool migrate_defaults = t["defaults"].value_or(int64_t(0)) < kDefaults;
    for (const Option& o : kGame.options) {
        if (o.flag) *o.flag = t[o.key].value_or(*o.flag);
        else *o.value = t[o.key].value_or(*o.value);
    }
    if (migrate_defaults) PhoneFrameDefaults(s);
    // Normalize simplified Motion Blur (Off / On), Display Logs (Off / Minimal / Full / Debug), and sync Dynamic Res FPS target
    game::Opt().motion_blur = game::Opt().motion_blur ? 1u : 0u;
    game::Opt().display_logs = std::min(game::Opt().display_logs, 3u);
    if (s.fps_cap >= 30u) game::Opt().dynamic_target_fps = s.fps_cap;
    if (kGame.load) kGame.load(&t);
}

void Save() {
    std::ofstream f(File());
    f << Table() << "\n";
}

void LogSettings() {
    std::ostringstream text;
    text << Table();
    std::istringstream lines(text.str());
    LOGR("Settings:");
    for (std::string line; std::getline(lines, line);)
        if (!line.empty()) LOGR("  %s", line.c_str());
}

void LogSystem() {
    std::time_t now = std::time(nullptr);
    char date[32] = {};
    std::strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    LOGR("%ls (%s), %s, build %s", kGame.title, IMAGE_XBE, date, BLACK_COMMIT);
    LOGR("Path: %s", ExeDir().c_str());
    LOGR("Device: %s %s, Android %s (API %d)", Property("ro.product.manufacturer").c_str(),
         Property("ro.product.model").c_str(), Property("ro.build.version.release").c_str(), SDL_GetAndroidSDKVersion());
    LOGR("CPU: %s (%d threads)", Property("ro.soc.model").c_str(), SDL_GetNumLogicalCPUCores());
    LOGR("RAM: %.1f GB", SDL_GetSystemRAM() / 1024.0);
    Mode desk = Desktop();
    LOGR("Display: %ux%u, %.0f Hz", desk.width, desk.height, RefreshRate());
    LogSettings();
}

void ApplyDisplay() {}

Mode Desktop() { return ScreenMode(); }

Mode Output() {
    // the window is the screen: the resolution setting is the screen's own
    return Desktop();
}

bool Widescreen() {
    Mode m = Output();
    return m.width * 2 >= m.height * 3;  // 3:2 or wider
}

// SSAA renders twice as wide and high, the present averaging each 2x2 back to a pixel
float RenderScale(uint32_t guest_height) {
    float ssaa = game::Opt().anti_aliasing == 4 ? 2.0f : 1.0f;
    float h = Output().height * (dynres::Percent() / 100.0f) * ssaa;
    return guest_height ? std::max(h / guest_height, 0.1f) : 1.0f;
}

// User-accessible shared storage (e.g. /storage/emulated/0/mc3dr) or app-specific external storage:
// stores saves (hdd/), settings, layout, shadercache and logs
std::filesystem::path ExeDir() {
    static const std::filesystem::path dir = [] {
        std::error_code ec;
        for (const char* candidate : {"/storage/emulated/0/mc3dr", "/sdcard/mc3dr"}) {
            if (std::filesystem::exists(candidate, ec)) {
                std::filesystem::path hdd = std::filesystem::path(candidate) / "hdd";
                std::filesystem::create_directories(hdd, ec);
                if (!ec) {
                    LOGI("app: ExeDir using user-accessible storage: %s (hdd at %s)", candidate, hdd.c_str());
                    return std::filesystem::path(candidate);
                }
            }
        }
        const char* p = nullptr;
        if (SDL_GetAndroidExternalStorageState() & SDL_ANDROID_EXTERNAL_STORAGE_WRITE) p = SDL_GetAndroidExternalStoragePath();
        if (!p) p = SDL_GetAndroidInternalStoragePath();
        return std::filesystem::path(p ? p : ".");
    }();
    return dir;
}

int Instance() { return 0; }

void* Window() { return g_window; }

// no splash and no relaunch into another executable on Android
bool OpenSplash() { return false; }
void SplashFirstFrame() {}
void CloseSplash() {}
void OpenGameWindow(bool) {}
bool StartHandover() { return false; }
bool HandingOver() { return false; }
void FinishHandover() {}

std::recursive_mutex& UiLock() {
    static std::recursive_mutex m;
    return m;
}

void Pace() {
    using clock = std::chrono::steady_clock;
    static clock::time_point next = clock::now();
    uint32_t cap = Get().fps_cap;
    auto now = clock::now();
    if (!cap) {
        next = now;
        return;
    }
    auto period = std::chrono::nanoseconds(1000000000ull / cap);
    next += period;
    if (next < now - period) next = now;
    while (clock::now() + std::chrono::milliseconds(2) < next) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    while (clock::now() < next) std::this_thread::yield();
}

}  // namespace app

namespace {

void Dispatch(const SDL_Event& e) {
    switch (e.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_TERMINATING:
            logx::Flush();
            kernel::Shutdown(0);
            break;
        case SDL_EVENT_WILL_ENTER_BACKGROUND:
            SetForeground(false);
            break;
        case SDL_EVENT_DID_ENTER_FOREGROUND:
            SetForeground(true);
            host::RefreshWindowPixels();
            break;
        case SDL_EVENT_WINDOW_RESIZED:
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            host::RefreshWindowPixels();
            break;
        case SDL_EVENT_LOW_MEMORY:
            LOGW("app: Android is low on memory");
            break;
        case SDL_EVENT_SENSOR_UPDATE:
        case SDL_EVENT_GAMEPAD_SENSOR_UPDATE:
            return;
        case SDL_EVENT_GAMEPAD_ADDED:
        case SDL_EVENT_GAMEPAD_REMOVED:
            input::OnPadEvent(e);
            break;
        default:
            break;
    }
    (void)touch::OnEvent(e);
}

}  // namespace

int main(int, char**) {
    logx::InstallCrashHandlers();
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO)) {
        char err[512];
        std::snprintf(err, sizeof(err), "SDL_Init failed: %s", SDL_GetError());
        __android_log_print(ANDROID_LOG_FATAL, "MC3DR", "%s", err);
        logx::SaveCrashReport("Fatal Startup Error", err);
        return 1;
    }
    logx::Init((app::ExeDir() / "logs").string(), app::kGame.log);
    app::Load();
    if (app::kGame.init) app::kGame.init();
    app::LogSystem();

    std::string title = Utf8(app::kGame.title);
    g_window = SDL_CreateWindow(title.c_str(), 0, 0, SDL_WINDOW_FULLSCREEN | SDL_WINDOW_VULKAN | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!g_window) {
        char err[512];
        std::snprintf(err, sizeof(err), "SDL_CreateWindow failed: %s", SDL_GetError());
        LOGE("app: no window: %s", SDL_GetError());
        logx::SaveCrashReport("Fatal Startup Error", err);
        return 1;
    }

    host::RefreshWindowPixels();

    app::GameFiles game;
    if (!app::FindGame(game)) {
        logx::SaveCrashReport("Fatal Startup Error", "FindGame could not find default.xbe or ISO in storage folders");
        return 1;
    }

    input::Init();
    if (app::kGame.window) app::kGame.window(g_window);
    if (!kernel::Boot(std::move(game.xbe), IMAGE_XBE, game.dir.string(), (app::ExeDir() / "hdd").string())) {
        logx::SaveCrashReport("Fatal Startup Error", "kernel::Boot failed to initialize game / memory / sections");
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, Utf8(app::kGame.short_title).c_str(),
                                 "The game failed to start, see crash_log.txt in the mc3dr folder.", g_window);
        return 1;
    }
    game::Loaded();

    uint64_t next_pump_ms = 0;
    for (;;) {
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, 100)) {
            Dispatch(e);
            while (SDL_PollEvent(&e)) Dispatch(e);
        }
        const uint64_t now_ms = SDL_GetTicks();
        if (now_ms >= next_pump_ms) {
            touch::Pump();
            next_pump_ms = now_ms + 100u;
        }
    }
}
