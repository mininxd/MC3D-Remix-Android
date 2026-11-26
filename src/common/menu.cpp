// Settings logic, FPS overlay, and option rows shared with the native in-game options menu
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <SDL3/SDL.h>
#include <imgui.h>

#include "app/app.h"
#include "audio/output.h"
#include "common/keys.h"
#include "common/menu.h"
#include "common/mouse_keyboard.h"
#include "common/options.h"
#include "common/translations.h"

// native/gyro.cpp: aiming by turning the phone
namespace gyro {
uint32_t CurrentMode();
const char* ModeName();
void SetMode(uint32_t mode);
float Sensitivity();
void SetSensitivity(float s);
bool InvertX();
bool InvertY();
void SetInvert(bool x, bool y);
}  // namespace gyro

// native/touch.cpp: the touch controls' layout
namespace touch {
const char* LayoutName();
void StepLayout(int step);
uint32_t ControlsOpacity();
void StepOpacity(int step);
uint32_t ControlsSize();
void StepSize(int step);
void EditLayout();
const char* SkinName();
void StepSkin(int step);
const char* StickModeName();
void StepStickMode(int step);
}  // namespace touch

// native/dynamic_resolution.cpp: the render resolution in use, percent of the screen's
namespace dynres {
uint32_t Percent();
}  // namespace dynres

// native/update.cpp: the activity asks GitHub for a newer release and says what it found
namespace host {
void CheckForUpdates();
bool StartupCheck();  // whether each launch asks GitHub
void SetStartupCheck(bool on);
}  // namespace host

namespace app {

Settings DefaultSettings();  // native/main.cpp: a phone's first run, unlimited frame rate and vsync off

namespace {

constexpr uint32_t kAnisoValues[] = {0, 2, 4, 8, 16};
const char* const kAnisoNames[] = {"Off", "2x", "4x", "8x", "16x"};
constexpr uint32_t kDistances[] = {1, 2, 4, 8};
const char* const kDistanceNames[] = {"Default", "2x", "4x", "8x"};
constexpr uint32_t kAntiAliasings[] = {0, 1, 2};
const char* const kAntiAliasingNames[] = {"Off", "FXAA", "SSAA 2x"};
constexpr uint32_t kFmvScales[] = {0, 1};
const char* const kFmvScaleNames[] = {"Fill", "4:3"};
constexpr uint32_t kScreenFilters[] = {0, 1, 2, 3, 4, 5, 6, 7, 8};
const char* const kScreenFilterNames[] = {"Off",  "CRT Light", "CRT Strong",   "Sharpen", "Vibrant",
                                          "Noir", "Sepia",     "Night Vision", "VHS"};
constexpr uint32_t kAudioOutputs[] = {audio::kOutputAuto, audio::kOutputMono, audio::kOutputStereo,
                                      audio::kOutputSurround};
const char* const kAudioOutputNames[] = {"AUTOMATIC", "MONO", "2.0 STEREO", "5.1 SURROUND"};
const char* const kLanguageNames[] = {"ENGLISH", "FRENCH", "GERMAN", "ITALIAN", "SPANISH",
                                      "INDONESIAN"};  // as game::kLanguages
constexpr uint32_t kFpsCaps[] = {30, 60, 90, 120, 144};

int StepIndex(int index, int step, int count) {
    if (count <= 0) return 0;
    index = (index + step) % count;
    return index < 0 ? index + count : index;
}

int NearestIndex(const uint32_t* values, int count, uint32_t value) {
    int best = 0;
    uint32_t best_d = UINT32_MAX;
    for (int i = 0; i < count; i++) {
        uint32_t d = values[i] > value ? values[i] - value : value - values[i];
        if (d < best_d) {
            best_d = d;
            best = i;
        }
    }
    return best;
}

// index nearest value, stepped first
template <size_t N>
int StepTable(const uint32_t (&values)[N], uint32_t& value, int step) {
    int i = NearestIndex(values, int(N), value);
    if (step) {
        i = StepIndex(i, step, int(N));
        value = values[i];
    }
    return i;
}

// value clamped to lo to hi, stepped by inc through the values StepIndex wraps over
int StepRange(int value, int lo, int hi, int inc, int step) {
    int count = (hi - lo) / inc + 1;
    return lo + inc * StepIndex((std::clamp(value, lo, hi) - lo) / inc, step, count);
}

const char* OnOff(bool on) { return on ? "On" : "Off"; }

void Flip(bool& on, int step) {
    if (step) on = !on;
}

uint32_t NormalizedMaxFps(uint32_t fps_cap) {
    return fps_cap >= 30u ? fps_cap : 60u;
}

void SyncDynamicResolutionBounds(game::Options& o, uint32_t fps_cap) {
    o.dynamic_min_percent = 30;
    o.dynamic_max_percent = 90;
    o.dynamic_target_fps = NormalizedMaxFps(fps_cap);
}

// applies and saves
void Changed() {
    audio::SetOutputMode(game::Opt().audio_output);
    Save();
}

enum RowId { kRenderResolution, kMaxFps, kVsync, kFpsCounter, kAniso, kReloadBlur, kSkipIntros, kSensitivity,
             kInvertMouse, kWalkSpeed, kFov, kViewmodelFov, kHoldAim, kDrawDistance, kAudioOutput, kLanguage,
             kFmvScale, kGyro, kGyroSensitivity, kTouchLayout, kTouchOpacity, kTouchSize, kTouchEdit,
             kCheckUpdates, kTouchSensitivity, kLeftDeadzone, kRightDeadzone, kLeftSensitivity, kRightSensitivity,
             kScreenFilter, kGyroInvertX, kGyroInvertY, kTouchSkin, kCredit, kBloom,
             kFog, kAntiAliasing, kTouchStick, kDynamicRes, kDynamicTarget, kDynamicMin,
             kDynamicMax, kDynamicHold, kAutoUpdate };

using rows::Kind;
using rows::Row;

// native Display Options rows (concise Audio Options style)
constexpr Row kDisplayRows[] = {
    {Kind::Value, kRenderResolution, "Render Scale"},
    {Kind::Value, kDynamicRes, "Dynamic Resolution"},
    {Kind::Value, kMaxFps, "Max FPS"},
    {Kind::Value, kScreenFilter, "Screen Filter"},
    {Kind::Value, kVsync, "VSync"},
    {Kind::Value, kFmvScale, "FMV Scale"},
};

// native Quality Options rows
constexpr Row kQualityRows[] = {
    {Kind::Value, kAntiAliasing, "Anti Aliasing"},
    {Kind::Value, kBloom, "Bloom"},
    {Kind::Value, kAniso, "Anisotropic Filtering"},
    {Kind::Value, kFog, "Fog"},
};

constexpr Row kTouchRows[] = {
    {Kind::Heading, -1, "TOUCH"},
    {Kind::Value, kTouchSensitivity, "TOUCH SENSITIVITY"},
    {Kind::Value, kTouchLayout, "CONTROLS LAYOUT"},
    {Kind::Value, kTouchOpacity, "CONTROLS OPACITY"},
    {Kind::Value, kTouchSize, "CONTROLS SIZE"},
    {Kind::Value, kTouchSkin, "BUTTON STYLE"},
    {Kind::Value, kTouchStick, "ANALOG STICK"},
    {Kind::Value, kTouchEdit, "EDIT CONTROLS POSITION"},
    {Kind::Heading, -1, "MOTION CONTROLS"},
    {Kind::Value, kGyro, "GYROSCOPE"},
    {Kind::Value, kGyroSensitivity, "GYRO SENSITIVITY"},
    {Kind::Value, kGyroInvertX, "GYRO INVERT X"},
    {Kind::Value, kGyroInvertY, "GYRO INVERT Y"},
};

// on the game's control setup page, under its own rows
constexpr Row kControllerRows[] = {
    {Kind::Value, kLeftDeadzone, "LEFT STICK DEADZONE"},
    {Kind::Value, kRightDeadzone, "RIGHT STICK DEADZONE"},
    {Kind::Value, kLeftSensitivity, "LEFT STICK SENSITIVITY"},
    {Kind::Value, kRightSensitivity, "RIGHT STICK SENSITIVITY"},
};

// LANGUAGE always shows: Indonesian is there whatever the disc
constexpr Row kGeneralRows[] = {
    {Kind::Value, kLanguage, "LANGUAGE"},
    {Kind::Value, kSkipIntros, "SKIP INTROS"},
    {Kind::Value, kAudioOutput, "AUDIO OUTPUT"},
    {Kind::Heading, -1, "UPDATES"},
    {Kind::Value, kCheckUpdates, "CHECK FOR UPDATES"},
    {Kind::Value, kAutoUpdate, "STARTUP UPDATE CHECK"},
};

// the title menu's CREDITS page: who and what, and the links, which open in the browser. Its rows take no step; a
// row's id is kCredit and its place in kCredits
struct Credit {
    const char* label;  // a heading when there is no value
    const char* value;
    const char* url;
};
constexpr Credit kCredits[] = {
    {"CONTRIBUTORS", nullptr, nullptr},
    {"Rangga Casper", "ANDROID PORT", "https://github.com/RanggaCasper"},
    {"BAGASROMADON", "BUTTON SKIN", "https://github.com/bagasromadon"},
    {"BUILT WITH", nullptr, nullptr},
    {"XBOXXBE-RECOMP", "TGP482", nullptr},
    {"SDL3, GLSLANG, TOML++", "LIBRARIES", nullptr},
    {"DEAR IMGUI", "LIBRARY", nullptr},
    {"XEMU, CXBX-RELOADED", "REFERENCES", nullptr},
    {"LINKS", nullptr, nullptr},
    {"YOUTUBE", "OPEN", "https://youtube.com/c/@RanggaCasper"},
    {"GITHUB", "OPEN", "https://github.com/RanggaCasper"},
    {"SUPPORT", "OPEN", "https://sociabuzz.com/racas/tribe"},
};

const std::vector<Row>& CreditRows() {
    static const std::vector<Row> list = [] {
        std::vector<Row> out;
        for (const Credit& c : kCredits) out.push_back({c.value ? Kind::Value : Kind::Heading, kCredit, c.label});
        return out;
    }();
    return list;
}

const Credit* CreditOf(const Row& row) {
    for (const Credit& c : kCredits)
        if (row.label == c.label) return &c;
    return nullptr;
}

const std::vector<Row>& InputRows() {
    static const std::vector<Row> list = [] {
        std::vector<Row> out = {{Kind::Heading, -1, "MOUSE"},
                                {Kind::Value, kSensitivity, "MOUSE SENSITIVITY"},
                                {Kind::Value, kInvertMouse, "INVERT LOOK"},
                                {Kind::Value, kHoldAim, "HOLD TO AIM"},
                                {Kind::Heading, -1, "KEYBOARD"}};
        size_t count = 0;
        const input::Action* actions = input::Actions(&count);
        for (size_t i = 0; i < count; i++) out.push_back({Kind::Bind, int(i), actions[i].label});
        out.push_back({Kind::Value, kWalkSpeed, "WALK SPEED"});
        return out;
    }();
    return list;
}

// the setting an ON / OFF row flips, nullptr for any other row
bool* ToggleFlag(Settings& s, int id) {
    game::Options& o = game::Opt();
    switch (id) {
        case kVsync: return &s.vsync;
        case kFpsCounter: return &o.fps_counter;
        case kReloadBlur: return &o.reload_blur;
        case kBloom: return &o.bloom;
        case kFog: return &o.fog;
        case kSkipIntros: return &o.skip_intros;
        case kInvertMouse: return &o.invert_mouse;
        case kHoldAim: return &o.hold_aim;
        case kDynamicRes: return &o.dynamic_resolution;
        default: return nullptr;
    }
}

// both field of view rows: 45 to 140 in steps of 1, 70 the game's own
constexpr int kFovMin = 45;
constexpr int kFovMax = 140;
constexpr uint32_t kGameFov = 70;

void FovText(uint32_t fov, char (&text)[64]) {
    std::snprintf(text, sizeof(text), fov == kGameFov ? "%u (DEFAULT)" : "%u", fov);
}

// the dynamic resolution's frame rates to hold
constexpr uint32_t kDynamicTargets[] = {30, 45, 60, 90, 120};

// a resolution as percent of the screen's and its pixels
void PercentText(uint32_t percent, char (&text)[64]) {
    if (percent == 0) {
        std::snprintf(text, sizeof(text), "0%% (MIN)");
        return;
    }
    Mode out = Output();
    std::snprintf(text, sizeof(text), "%u%% (%ux%u)", percent, out.width * percent / 100, out.height * percent / 100);
}

// the display rows that are no ON / OFF, stepped first; false for another row
bool StepDisplayRow(int id, int step, Settings& s, char (&text)[64]) {
    game::Options& o = game::Opt();
    switch (id) {
        case kRenderResolution: {
            // 25 to 125 in steps of 5 (default 75); adjusting switches Dynamic Resolution off cleanly
            if (step) {
                if (o.dynamic_resolution) o.dynamic_resolution = false;
                s.render_percent = uint32_t(StepRange(int(s.render_percent), 25, 125, 5, step));
            }
            const uint32_t shown = o.dynamic_resolution ? dynres::Percent()
                                                        : std::clamp(s.render_percent, 25u, 125u);
            std::snprintf(text, sizeof(text), "%u", shown);
            return true;
        }
        case kDynamicTarget:
            std::snprintf(text, sizeof(text), "%u FPS", kDynamicTargets[StepTable(kDynamicTargets, o.dynamic_target_fps, step)]);
            return true;
        case kDynamicMin:
            // 20% up to the max, the max down to the min
            if (step) o.dynamic_min_percent = uint32_t(StepRange(int(o.dynamic_min_percent), 20, int(o.dynamic_max_percent), 5, step));
            PercentText(o.dynamic_min_percent, text);
            return true;
        case kDynamicMax:
            if (step) o.dynamic_max_percent = uint32_t(StepRange(int(o.dynamic_max_percent), int(o.dynamic_min_percent), 200, 5, step));
            PercentText(o.dynamic_max_percent, text);
            return true;
        case kDynamicHold:
            // 1 to 30 seconds
            if (step) o.dynamic_hold = uint32_t(StepRange(int(o.dynamic_hold), 1, 30, 1, step));
            std::snprintf(text, sizeof(text), "%u S", o.dynamic_hold);
            return true;
        case kFmvScale:
            std::snprintf(text, sizeof(text), "%s", kFmvScaleNames[StepTable(kFmvScales, o.fmv_scale, step)]);
            return true;
        case kAntiAliasing:
            std::snprintf(text, sizeof(text), "%s",
                          kAntiAliasingNames[StepTable(kAntiAliasings, o.anti_aliasing, step)]);
            return true;
        case kScreenFilter:
            std::snprintf(text, sizeof(text), "%s",
                          kScreenFilterNames[StepTable(kScreenFilters, o.screen_filter, step)]);
            return true;
        case kFov:
            if (step) o.fov = uint32_t(StepRange(int(o.fov), kFovMin, kFovMax, 1, step));
            FovText(o.fov, text);
            return true;
        case kViewmodelFov:
            if (step) o.viewmodel_fov = uint32_t(StepRange(int(o.viewmodel_fov), kFovMin, kFovMax, 1, step));
            FovText(o.viewmodel_fov, text);
            return true;
        case kMaxFps: {
            uint32_t current = NormalizedMaxFps(s.fps_cap);
            uint32_t cap = kFpsCaps[StepTable(kFpsCaps, current, step)];
            if (step) {
                s.fps_cap = cap;
                o.dynamic_target_fps = cap;
            }
            std::snprintf(text, sizeof(text), "%u", cap);
            return true;
        }
        default:
            return false;
    }
}

// the graphics rows that are no ON / OFF, stepped first; false for another row
bool StepGraphicsRow(int id, int step, Settings& s, char (&text)[64]) {
    game::Options& o = game::Opt();
    switch (id) {
        case kAniso:
            std::snprintf(text, sizeof(text), "%s", kAnisoNames[StepTable(kAnisoValues, s.aniso, step)]);
            return true;
        case kDrawDistance:
            std::snprintf(text, sizeof(text), "%s", kDistanceNames[StepTable(kDistances, o.draw_distance, step)]);
            return true;
        default:
            return false;
    }
}

// the general, mouse and keyboard rows that are no ON / OFF, stepped first
void StepInputRow(int id, int step, char (&text)[64]) {
    game::Options& o = game::Opt();
    switch (id) {
        case kAudioOutput:
            std::snprintf(text, sizeof(text), "%s", kAudioOutputNames[StepTable(kAudioOutputs, o.audio_output, step)]);
            break;
        case kSensitivity:
            // 0.1 to 5.0 in steps of 0.1
            if (step) {
                int tenths = StepRange(int(std::lround(o.mouse_sensitivity * 10.0f)), 1, 50, 1, step);
                o.mouse_sensitivity = float(tenths) / 10.0f;
            }
            std::snprintf(text, sizeof(text), "%.1f", o.mouse_sensitivity);
            break;
        case kTouchSensitivity:
            // as the mouse's: 0.1 to 5.0 in steps of 0.1
            if (step) {
                int tenths = StepRange(int(std::lround(o.touch_sensitivity * 10.0f)), 1, 50, 1, step);
                o.touch_sensitivity = float(tenths) / 10.0f;
            }
            std::snprintf(text, sizeof(text), "%.1f", o.touch_sensitivity);
            break;
        case kWalkSpeed:
            // 10% to 100% in steps of 5
            if (step) o.walk_speed = uint32_t(StepRange(int(o.walk_speed), 10, 100, 5, step));
            std::snprintf(text, sizeof(text), "%u%%", o.walk_speed);
            break;
        case kGyro:
            // off, always on, while firing
            if (step) gyro::SetMode(uint32_t(StepIndex(int(gyro::CurrentMode()), step, 3)));
            std::snprintf(text, sizeof(text), "%s", gyro::CurrentMode() == 0 ? "OFF"
                                                    : gyro::CurrentMode() == 1 ? "ALWAYS"
                                                                               : "WHILE FIRING");
            break;
        case kTouchLayout:
            // the presets, CUSTOM once a control was moved
            if (step) touch::StepLayout(step);
            std::snprintf(text, sizeof(text), "%s", touch::LayoutName());
            break;
        case kTouchOpacity:
            if (step) touch::StepOpacity(step);
            std::snprintf(text, sizeof(text), "%u%%", touch::ControlsOpacity());
            break;
        case kTouchSize:
            if (step) touch::StepSize(step);
            std::snprintf(text, sizeof(text), "%u%%", touch::ControlsSize());
            break;
        case kTouchSkin:
            if (step) touch::StepSkin(step);
            std::snprintf(text, sizeof(text), "%s", touch::SkinName());
            break;
        case kTouchStick:
            // under the thumb where it comes down, or fixed in its place
            if (step) touch::StepStickMode(step);
            std::snprintf(text, sizeof(text), "%s", touch::StickModeName());
            break;
        case kTouchEdit:
            // any step opens the editor over the screen
            if (step) touch::EditLayout();
            std::snprintf(text, sizeof(text), "OPEN");
            break;
        case kCheckUpdates:
            // any step asks; the answer comes in a dialog over the game
            if (step) host::CheckForUpdates();
            std::snprintf(text, sizeof(text), "CHECK");
            break;
        case kGyroSensitivity:
            // 0.1 to 5.0 in steps of 0.1
            if (step) gyro::SetSensitivity(float(StepRange(int(std::lround(gyro::Sensitivity() * 10.0f)), 1, 50, 1, step)) / 10.0f);
            std::snprintf(text, sizeof(text), "%.1f", gyro::Sensitivity());
            break;
        case kGyroInvertX:
        case kGyroInvertY: {
            // kept with the gyro's other settings in android.toml, so not one of ToggleFlag's
            bool x = gyro::InvertX(), y = gyro::InvertY();
            if (step) (id == kGyroInvertX ? x : y) ^= true;
            if (step) gyro::SetInvert(x, y);
            std::snprintf(text, sizeof(text), "%s", OnOff(id == kGyroInvertX ? x : y));
            break;
        }
        case kLeftDeadzone:
        case kRightDeadzone: {
            // 0% to 50% of the stick's travel in steps of 1, 0 leaves the game's own
            uint32_t& dz = id == kLeftDeadzone ? o.left_deadzone : o.right_deadzone;
            if (step) dz = uint32_t(StepRange(int(dz), 0, 50, 1, step));
            std::snprintf(text, sizeof(text), dz ? "%u%%" : "%u%% (DEFAULT)", dz);
            break;
        }
        case kLeftSensitivity:
        case kRightSensitivity: {
            // 0.1 to 3.0 in steps of 0.1
            float& s = id == kLeftSensitivity ? o.left_sensitivity : o.right_sensitivity;
            if (step) s = float(StepRange(int(std::lround(s * 10.0f)), 1, 30, 1, step)) / 10.0f;
            std::snprintf(text, sizeof(text), "%.1f", s);
            break;
        }
        case kLanguage: {
            int count = int(std::size(game::kLanguages));
            int i = 0;
            while (i < count - 1 && o.language != game::kLanguages[i]) i++;
            // past the languages this disc has no strings for; English and Indonesian are always there
            if (step) {
                do i = StepIndex(i, step, count);
                while (!game::LanguageAvailable(game::kLanguages[i]));
                o.language = game::kLanguages[i];
            }
            const char* format = o.language == game::BootLanguage() ? "%s" : "%s (RESTART)";
            std::snprintf(text, sizeof(text), format, kLanguageNames[i]);
            break;
        }
        case kAutoUpdate:
            // kept in the activity's preferences: it is read at launch, before the game's settings
            if (step) host::SetStartupCheck(!host::StartupCheck());
            std::snprintf(text, sizeof(text), "%s", OnOff(host::StartupCheck()));
            break;
    }
}

// steps the value first when step is not zero
std::string RowText(int id, int step, Settings& s = Get()) {
    char text[64] = "";
    if (bool* on = ToggleFlag(s, id)) {
        Flip(*on, step);
        if (step && id == kDynamicRes && *on) {
            SyncDynamicResolutionBounds(game::Opt(), s.fps_cap);
        }
        std::snprintf(text, sizeof(text), "%s", OnOff(*on));
    } else if (!StepDisplayRow(id, step, s, text) && !StepGraphicsRow(id, step, s, text)) {
        StepInputRow(id, step, text);
    }
    if (step && &s == &Get()) Changed();
    return text;
}

constexpr const char* kUnbound = "UNBOUND";

std::string KeyText(std::string keys) {
    if (keys.empty()) return kUnbound;
    for (char& c : keys) c = c == ',' ? '/' : char(std::toupper(uint8_t(c)));
    return keys;
}

constexpr int kTouchPage = 1;
constexpr int kGeneralPage = 2;
constexpr int kKeyboardPage = 3;
constexpr int kCreditsPage = 5;

void RestoreDefaults(int page) {
    game::Options& o = game::Opt();
    if (page == kKeyboardPage) {
        o.mouse_sensitivity = game::Options{}.mouse_sensitivity;
        o.invert_mouse = false;
        o.hold_aim = true;
        o.walk_speed = game::Options{}.walk_speed;
        input::ResetBindings();
        return;
    }
    game::Options keep = o;
    Get() = DefaultSettings();
    o = game::Options{};
    o.mouse_sensitivity = keep.mouse_sensitivity;
    o.invert_mouse = keep.invert_mouse;
    o.hold_aim = keep.hold_aim;
    o.walk_speed = keep.walk_speed;
    o.keys = keep.keys;
    Changed();
}

// the window's display's refresh rate, 0 when unreadable
uint32_t RefreshRate() {
    SDL_Window* w = static_cast<SDL_Window*>(Window());
    const SDL_DisplayMode* m = SDL_GetCurrentDisplayMode(w ? SDL_GetDisplayForWindow(w) : SDL_GetPrimaryDisplay());
    return m && m->refresh_rate > 1 ? uint32_t(std::lround(m->refresh_rate)) : 0;
}

// as app::Pace, at this cap
void PaceAt(uint32_t cap) {
    using clock = std::chrono::steady_clock;
    static clock::time_point next = clock::now();
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

}  // namespace

void PaceFrame() {
    if (Get().fps_cap != kFpsCapRefresh) {
        Pace();
        return;
    }
    // a few under, so a variable refresh display stays in its range and vsync never waits a whole refresh
    constexpr uint32_t kRefreshMargin = 3;
    uint32_t rate = RefreshRate();
    PaceAt(rate > kRefreshMargin ? rate - kRefreshMargin : 0);
}

const rows::Row* rows::Page(int page, size_t* count) {
    if (page == kKeyboardPage) {
        *count = InputRows().size();
        return InputRows().data();
    }
    if (page == kCreditsPage) {
        *count = CreditRows().size();
        return CreditRows().data();
    }
    if (page == kTouchPage) {
        *count = std::size(kTouchRows);
        return kTouchRows;
    }
    if (page == kGeneralPage) {
        *count = std::size(kGeneralRows);
        return kGeneralRows;
    }
    if (page == rows::kQualityPage) {
        *count = std::size(kQualityRows);
        return kQualityRows;
    }
    *count = std::size(kDisplayRows);
    return kDisplayRows;
}

const rows::Row* rows::Controller(size_t* count) {
    *count = std::size(kControllerRows);
    return kControllerRows;
}

std::string rows::Value(const Row& row, int step) {
    if (row.kind == Kind::Bind) return KeyText(input::Binding(size_t(row.id)));
    if (row.id == kCredit) {
        const Credit* c = CreditOf(row);
        if (!c || !c->value) return " ";
        if (step && c->url) SDL_OpenURL(c->url);
        return c->value;
    }
    return RowText(row.id, step);
}

void rows::Defaults(int page) { RestoreDefaults(page); }

bool rows::Restorable(int page) { return page == rows::kDisplayPage || page == rows::kQualityPage || page == kKeyboardPage; }

bool rows::Enabled(const Row& row) {
    if (row.kind != Kind::Value) return true;
    bool dynamic = game::Opt().dynamic_resolution;
    switch (row.id) {
        case kDynamicTarget:
        case kDynamicMin:
        case kDynamicMax:
        case kDynamicHold: return dynamic;
        default: return true;
    }
}

bool rows::Opens(const Row& row) {
    return row.kind == Kind::Value && (row.id == kTouchEdit || row.id == kCheckUpdates || row.id == kCredit);
}

bool rows::SliderRatio(int id, float* out_ratio) {
    if (!out_ratio) return false;
    const game::Options& o = game::Opt();
    const Settings& s = Get();
    float ratio = 0.0f;
    switch (id) {
        case kRenderResolution:
            ratio = float(o.dynamic_resolution ? dynres::Percent() : s.render_percent) / 125.0f;
            break;
        case kFov:
            ratio = float(int(o.fov) - kFovMin) / float(kFovMax - kFovMin);
            break;
        case kViewmodelFov:
            ratio = float(int(o.viewmodel_fov) - kFovMin) / float(kFovMax - kFovMin);
            break;
        case kDynamicMin: {
            float span = float(std::max<int>(1, int(o.dynamic_max_percent) - 20));
            ratio = float(int(o.dynamic_min_percent) - 20) / span;
            break;
        }
        case kDynamicMax: {
            float span = float(std::max<int>(1, 200 - int(o.dynamic_min_percent)));
            ratio = float(int(o.dynamic_max_percent) - int(o.dynamic_min_percent)) / span;
            break;
        }
        case kDynamicHold:
            ratio = float(int(o.dynamic_hold) - 1) / 29.0f;
            break;
        default:
            return false;
    }
    *out_ratio = std::clamp(ratio, 0.0f, 1.0f);
    return true;
}

bool MenuOpen() { return false; }

void DrawUi(double fps) {
    game::g_fps = float(fps);
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
}

}  // namespace app
