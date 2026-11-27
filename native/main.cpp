// The Android host: SDL's window and event loop, the settings and folders, the app's lifecycle; the game is described
// by app::kGame (src/common/game_info.cpp)
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <android/log.h>
#include <sys/system_properties.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
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

void WindowPixels(int& w, int& h) {
    w = h = 0;
    if (g_window) SDL_GetWindowSizeInPixels(g_window, &w, &h);
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
    float ssaa = game::Opt().anti_aliasing == 2 ? 2.0f : 1.0f;
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
            break;
        case SDL_EVENT_LOW_MEMORY:
            LOGW("app: Android is low on memory");
            break;
        case SDL_EVENT_SENSOR_UPDATE:
        case SDL_EVENT_GAMEPAD_SENSOR_UPDATE:
            gyro::OnEvent(e);
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
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO | SDL_INIT_SENSOR)) {
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

    for (;;) {
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, 100)) {
            Dispatch(e);
            while (SDL_PollEvent(&e)) Dispatch(e);
        }
        touch::Pump();
    }
}
