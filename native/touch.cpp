// Android touch controls, keyboard/mouse bridge, SVG skin rasterizer (LunaSVG), and interactive layout editor.
// Context-aware minimal mobile racing HUD for Midnight Club 3: DUB Edition Remix:
//   - Main / Pause Menu : D-Pad + OK + Back
//   - Garage            : D-Pad Left/Right + OK + Back (+ vertical swipe for upgrade lists)
//   - Gameplay          : 1:2 Steer Left/Right (with thumb-slide), 1:2 Gas, 2:3 Brake, Handbrake,
//                         dynamic NOS (only when purchased), dynamic Slipstream Turbo (only when filled & speed >= 10 km/h),
//                         dynamic Special Ability (Zone/Agro/Roar), minimal top bar, and bottom-center Radio swipe.
#include "common/mouse_keyboard.h"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <lunasvg.h>
#include <toml++/toml.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "app/app.h"
#include "common/aspect.h"
#include "common/keys.h"
#include "common/menu.h"
#include "common/options.h"
#include "common/translations.h"
#include "core/log.h"
#include "game/touch_context.h"
#include "host.h"

namespace input {

namespace {

std::atomic<int> g_dx{0};
std::atomic<int> g_dy{0};
std::atomic<int> g_touch_dx{0};
std::atomic<int> g_touch_dy{0};
std::atomic<uint64_t> g_take_ms{0};
std::atomic<uint64_t> g_capture_ms{0};
std::atomic<bool> g_settle{false};
std::atomic<uint64_t> g_confirm_until{0};
std::atomic<bool> g_menu{false};
std::atomic<bool> g_keyboard{false};
std::atomic<bool> g_touch{true};
std::atomic<bool> g_text{false};
std::atomic<bool> g_text_shown{false};
std::mutex g_chars_lock;
std::u16string g_chars;
constexpr size_t kMaxChars = 64;
std::atomic<int> g_wheel{0};
constexpr uint64_t kPlayMs = 250;
constexpr uint64_t kTapMs = 80;
constexpr uint64_t kCaptureMs = 250;
constexpr int kWheelUp = 0x0E;
constexpr int kWheelDown = 0x0F;
std::atomic<uint64_t> g_wheel_ms[2] = {};
std::array<std::atomic<bool>, 256> g_down{};

uint64_t NowMs() {
    auto since = std::chrono::steady_clock::now().time_since_epoch();
    return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(since).count()) + 1;
}

struct Target {
    Action action;
    int button;
};
constexpr int kHostKey = -2;
constexpr int kWalk = -3;
constexpr int kFirstButtonBit = 8;
constexpr int kStart = 12;
constexpr int kFirstMoveKey = 16;
constexpr int kFirstLookKey = 20;
constexpr int kStickMax = 32767;

constexpr Target kActions[] = {
    {{"move_forward", "MOVE FORWARD", "W"}, 16},
    {{"move_back", "MOVE BACK", "S"}, 17},
    {{"strafe_left", "STRAFE LEFT", "A"}, 18},
    {{"strafe_right", "STRAFE RIGHT", "D"}, 19},
    {{"walk", "WALK", "Alt"}, kWalk},
    {{"crouch", "SPECIAL", "C"}, 14},
    {{"fire", "GAS", "LMB"}, 7},
    {{"aim", "LOOK", "RMB"}, 15},
    {{"melee", "WEIGHT TRANSFER", "V"}, 1},
    {{"reload", "HANDBRAKE", "R"}, 0},
    {{"interact", "NITRO", "F"}, 2},
    {{"grenade", "BRAKE", "G"}, 6},
    {{"weapon_up", "RADIO NEXT", "E,Wheel Up"}, 11},
    {{"weapon_down", "RADIO PREV", "Q,Wheel Down"}, 10},
    {{"suppressor", "LOOK BACK", "T"}, 3},
    {{"firing_mode", "DPAD UP", "X"}, 8},
    {{"health_pack", "DPAD DOWN", "H"}, 9},
    {{"next_track", "FLASH LIGHTS", "N"}, 5},
    {{"back", "MAP", "Tab"}, 13},
    {{"pause", "PAUSE MENU", "Escape"}, 12},
    {{"hide_hud", "TOGGLE HUD", ""}, kHostKey},
    {{"settings_panel", "SETTINGS PANEL", "F10"}, kHostKey},
    {{"look_up", "LOOK UP", "Up"}, 20},
    {{"look_down", "LOOK DOWN", "Down"}, 21},
    {{"look_left", "LOOK LEFT", "Left"}, 22},
    {{"look_right", "LOOK RIGHT", "Right"}, 23},
};
constexpr size_t kCount = std::size(kActions);
constexpr size_t HostAction(std::string_view id) {
    size_t i = 0;
    while (kActions[i].action.id != id) i++;
    return i;
}
constexpr size_t kHudAction = HostAction("hide_hud");

struct Name {
    int vk;
    const char* name;
};
constexpr Name kNames[] = {
    {VK_LBUTTON, "LMB"}, {VK_RBUTTON, "RMB"}, {VK_MBUTTON, "MMB"}, {VK_XBUTTON1, "Mouse4"}, {VK_XBUTTON2, "Mouse5"},
    {kWheelUp, "Wheel Up"}, {kWheelDown, "Wheel Down"},
    {VK_SPACE, "Space"}, {VK_RETURN, "Enter"}, {VK_ESCAPE, "Escape"}, {VK_BACK, "Backspace"}, {VK_TAB, "Tab"},
    {VK_SHIFT, "Shift"}, {VK_CONTROL, "Ctrl"}, {VK_MENU, "Alt"}, {VK_CAPITAL, "CapsLock"},
    {VK_UP, "Up"}, {VK_DOWN, "Down"}, {VK_LEFT, "Left"}, {VK_RIGHT, "Right"},
    {VK_INSERT, "Insert"}, {VK_DELETE, "Delete"}, {VK_HOME, "Home"}, {VK_END, "End"},
    {VK_PRIOR, "PageUp"}, {VK_NEXT, "PageDown"},
    {'A', "A"}, {'B', "B"}, {'C', "C"}, {'D', "D"}, {'E', "E"}, {'F', "F"}, {'G', "G"}, {'H', "H"}, {'I', "I"},
    {'J', "J"}, {'K', "K"}, {'L', "L"}, {'M', "M"}, {'N', "N"}, {'O', "O"}, {'P', "P"}, {'Q', "Q"}, {'R', "R"},
    {'S', "S"}, {'T', "T"}, {'U', "U"}, {'V', "V"}, {'W', "W"}, {'X', "X"}, {'Y', "Y"}, {'Z', "Z"},
    {'0', "0"}, {'1', "1"}, {'2', "2"}, {'3', "3"}, {'4', "4"}, {'5', "5"}, {'6', "6"}, {'7', "7"}, {'8', "8"},
    {'9', "9"},
    {VK_F1, "F1"}, {VK_F2, "F2"}, {VK_F4, "F4"}, {VK_F5, "F5"}, {VK_F6, "F6"}, {VK_F7, "F7"},
    {VK_F8, "F8"}, {VK_F9, "F9"}, {VK_F10, "F10"}, {VK_F11, "F11"}, {VK_F12, "F12"},
    {VK_NUMPAD0, "Numpad0"}, {VK_NUMPAD1, "Numpad1"}, {VK_NUMPAD2, "Numpad2"}, {VK_NUMPAD3, "Numpad3"},
    {VK_NUMPAD4, "Numpad4"}, {VK_NUMPAD5, "Numpad5"}, {VK_NUMPAD6, "Numpad6"}, {VK_NUMPAD7, "Numpad7"},
    {VK_NUMPAD8, "Numpad8"}, {VK_NUMPAD9, "Numpad9"}, {VK_ADD, "NumpadPlus"}, {VK_SUBTRACT, "NumpadMinus"},
    {VK_MULTIPLY, "NumpadStar"}, {VK_DIVIDE, "NumpadSlash"}, {VK_DECIMAL, "NumpadPeriod"},
    {VK_OEM_3, "Backtick"}, {VK_OEM_MINUS, "Minus"}, {VK_OEM_PLUS, "Equals"}, {VK_OEM_4, "LBracket"},
    {VK_OEM_6, "RBracket"}, {VK_OEM_5, "Backslash"}, {VK_OEM_1, "Semicolon"}, {VK_OEM_7, "Quote"},
    {VK_OEM_COMMA, "Comma"}, {VK_OEM_PERIOD, "Period"}, {VK_OEM_2, "Slash"},
};
constexpr int kFixed[] = {VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, VK_RETURN, VK_BACK};

using Binds = std::array<std::vector<int>, kCount>;
std::mutex g_bind_lock;
Binds g_binds;

int Vk(const std::string& name) {
    for (const Name& n : kNames)
        if (strcasecmp(n.name, name.c_str()) == 0) return n.vk;
    return 0;
}

const char* NameOf(int vk) {
    for (const Name& n : kNames)
        if (n.vk == vk) return n.name;
    return nullptr;
}

std::vector<int> Parse(const std::string& list) {
    std::vector<int> out;
    size_t start = 0;
    while (start <= list.size()) {
        size_t comma = list.find(',', start);
        if (comma == std::string::npos) comma = list.size();
        if (int vk = Vk(list.substr(start, comma - start))) out.push_back(vk);
        start = comma + 1;
    }
    return out;
}

std::string Join(const std::vector<int>& vks) {
    std::string s;
    for (int vk : vks) {
        if (!s.empty()) s += ',';
        s += NameOf(vk);
    }
    return s;
}

bool Active() { return host::Foreground() && !app::MenuOpen(); }
bool Wheel(int vk) { return vk == kWheelUp || vk == kWheelDown; }
bool Down(int vk) {
    if (Wheel(vk)) return NowMs() - g_wheel_ms[vk == kWheelDown] < kTapMs;
    return vk > 0 && vk < 256 && g_down[size_t(vk)];
}
bool Mouse(int vk) {
    bool button = vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON || vk == VK_XBUTTON1 || vk == VK_XBUTTON2;
    return button || Wheel(vk);
}
bool Play() { return NowMs() - g_take_ms < kPlayMs; }
bool GameDown(int vk) { return Down(vk) && !(g_menu && vk == VK_DELETE); }
bool KeyCaptureUp() { return NowMs() - g_capture_ms <= kCaptureMs; }

bool HostKeyBound(size_t action, int vk) {
    if (KeyCaptureUp()) return false;
    std::lock_guard lock(g_bind_lock);
    const auto& b = g_binds[action];
    return std::find(b.begin(), b.end(), vk) != b.end();
}

bool UpdateSettle(const Binds& binds) {
    if (!g_settle) return true;
    for (auto& b : binds)
        for (int vk : b)
            if (Down(vk)) return false;
    for (int vk : kFixed)
        if (Down(vk)) return false;
    g_settle = false;
    return true;
}

struct Held {
    bool used = false;
    bool walk = false;
    int stick[8] = {};
};

Held PressActions(const Binds& binds, bool play, Pad& p) {
    Held held;
    auto pressed = [play](int vk) { return GameDown(vk) && (play || !Wheel(vk)); };
    static bool pause_held;
    static bool pause_backs;
    bool pause = false;
    for (size_t i = 0; i < kCount; i++) {
        int b = kActions[i].button;
        if (b == kHostKey || !std::any_of(binds[i].begin(), binds[i].end(), pressed)) continue;
        held.used = true;
        if (g_text && b != kStart) continue;
        if (b == kWalk) {
            held.walk = true;
        } else if (b >= kFirstLookKey && !play) {
            continue;
        } else if (b >= kFirstMoveKey) {
            held.stick[b - kFirstMoveKey] = 1;
        } else if (b < kFirstButtonBit) {
            p.analog[b] = 0xFF;
        } else if (b == kStart) {
            if (!pause_held) pause_backs = g_menu;
            pause = true;
            if (!pause_backs) p.buttons |= uint16_t(1u << (b - kFirstButtonBit));
            else if (g_menu) p.analog[1] = 0xFF;
        } else {
            p.buttons |= uint16_t(1u << (b - kFirstButtonBit));
        }
    }
    pause_held = pause;
    return held;
}

int VkOf(SDL_Scancode s) {
    if (s >= SDL_SCANCODE_A && s <= SDL_SCANCODE_Z) return 'A' + (s - SDL_SCANCODE_A);
    if (s >= SDL_SCANCODE_1 && s <= SDL_SCANCODE_9) return '1' + (s - SDL_SCANCODE_1);
    if (s >= SDL_SCANCODE_F1 && s <= SDL_SCANCODE_F12) return VK_F1 + (s - SDL_SCANCODE_F1);
    if (s >= SDL_SCANCODE_KP_1 && s <= SDL_SCANCODE_KP_9) return VK_NUMPAD1 + (s - SDL_SCANCODE_KP_1);
    switch (s) {
        case SDL_SCANCODE_0: return '0';
        case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: return VK_RETURN;
        case SDL_SCANCODE_ESCAPE: return VK_ESCAPE;
        case SDL_SCANCODE_BACKSPACE: return VK_BACK;
        case SDL_SCANCODE_TAB: return VK_TAB;
        case SDL_SCANCODE_SPACE: return VK_SPACE;
        case SDL_SCANCODE_LSHIFT: case SDL_SCANCODE_RSHIFT: return VK_SHIFT;
        case SDL_SCANCODE_LCTRL: case SDL_SCANCODE_RCTRL: return VK_CONTROL;
        case SDL_SCANCODE_LALT: case SDL_SCANCODE_RALT: return VK_MENU;
        case SDL_SCANCODE_CAPSLOCK: return VK_CAPITAL;
        case SDL_SCANCODE_UP: return VK_UP;
        case SDL_SCANCODE_DOWN: return VK_DOWN;
        case SDL_SCANCODE_LEFT: return VK_LEFT;
        case SDL_SCANCODE_RIGHT: return VK_RIGHT;
        case SDL_SCANCODE_INSERT: return VK_INSERT;
        case SDL_SCANCODE_DELETE: return VK_DELETE;
        case SDL_SCANCODE_HOME: return VK_HOME;
        case SDL_SCANCODE_END: return VK_END;
        case SDL_SCANCODE_PAGEUP: return VK_PRIOR;
        case SDL_SCANCODE_PAGEDOWN: return VK_NEXT;
        case SDL_SCANCODE_KP_0: return VK_NUMPAD0;
        case SDL_SCANCODE_KP_PLUS: return VK_ADD;
        case SDL_SCANCODE_KP_MINUS: return VK_SUBTRACT;
        case SDL_SCANCODE_KP_MULTIPLY: return VK_MULTIPLY;
        case SDL_SCANCODE_KP_DIVIDE: return VK_DIVIDE;
        case SDL_SCANCODE_KP_PERIOD: return VK_DECIMAL;
        case SDL_SCANCODE_GRAVE: return VK_OEM_3;
        case SDL_SCANCODE_MINUS: return VK_OEM_MINUS;
        case SDL_SCANCODE_EQUALS: return VK_OEM_PLUS;
        case SDL_SCANCODE_LEFTBRACKET: return VK_OEM_4;
        case SDL_SCANCODE_RIGHTBRACKET: return VK_OEM_6;
        case SDL_SCANCODE_BACKSLASH: return VK_OEM_5;
        case SDL_SCANCODE_SEMICOLON: return VK_OEM_1;
        case SDL_SCANCODE_APOSTROPHE: return VK_OEM_7;
        case SDL_SCANCODE_COMMA: return VK_OEM_COMMA;
        case SDL_SCANCODE_PERIOD: return VK_OEM_PERIOD;
        case SDL_SCANCODE_SLASH: return VK_OEM_2;
        default: return 0;
    }
}

int VkOfButton(uint8_t b) {
    switch (b) {
        case SDL_BUTTON_LEFT: return VK_LBUTTON;
        case SDL_BUTTON_RIGHT: return VK_RBUTTON;
        case SDL_BUTTON_MIDDLE: return VK_MBUTTON;
        case SDL_BUTTON_X1: return VK_XBUTTON1;
        case SDL_BUTTON_X2: return VK_XBUTTON2;
        default: return 0;
    }
}

}  // namespace

void ReleaseCursor() { g_settle = true; }

bool TakeMouse(float& dx, float& dy) {
    uint64_t now = NowMs();
    bool stale = now - g_take_ms.exchange(now) >= kPlayMs;
    int x = g_dx.exchange(0), tx = g_touch_dx.exchange(0);
    int y = g_dy.exchange(0), ty = g_touch_dy.exchange(0);
    if ((!x && !y && !tx && !ty) || stale || !Active()) return false;
    const game::Options& o = game::Opt();
    dx = float(x) * o.mouse_sensitivity + float(tx) * o.touch_sensitivity;
    dy = float(y) * o.mouse_sensitivity + float(ty) * o.touch_sensitivity;
    if (o.invert_mouse) dy = -dy;
    return true;
}

void Keyboard(Pad& p) {
    if (!Active()) {
        g_settle = true;
        return;
    }
    Binds binds;
    {
        std::lock_guard lock(g_bind_lock);
        binds = g_binds;
    }
    if (!UpdateSettle(binds)) return;
    bool play = Play();
    bool fixed_down = std::any_of(std::begin(kFixed), std::end(kFixed), Down);
    Held held = PressActions(binds, play, p);
    bool confirm = !play && !g_text && (Down(VK_RETURN) || Down(VK_SPACE));
    if (confirm || NowMs() < g_confirm_until) p.analog[0] = 0xFF;
    if ((!play && !g_text && Down(VK_BACK)) || (g_menu && Down(VK_RBUTTON))) p.analog[1] = 0xFF;
    if (!play) {
        for (int i = 0; i < 4; i++) p.buttons |= uint16_t(Down(kFixed[i]) << i);
    }
    const int* stick = held.stick;
    float speed = held.walk ? float(game::Opt().walk_speed) / 100.0f : 1.0f;
    if (int x = stick[3] - stick[2]) p.lx = int16_t(float(x) * speed * float(kStickMax));
    if (int y = stick[0] - stick[1]) p.ly = int16_t(float(y) * speed * float(kStickMax));
    if (int x = stick[7] - stick[6]) p.rx = int16_t(x * kStickMax);
    if (int y = stick[4] - stick[5]) p.ry = int16_t(y * kStickMax);
    if (fixed_down || held.used) g_keyboard = true;
}

void SeenPad(const Pad& p) {
    constexpr int16_t kStickPushed = 16000;
    constexpr uint8_t kAnalogPushed = 0x20;
    auto pushed = [](int16_t v) { return v > kStickPushed || v < -kStickPushed; };
    auto pressed = [](uint8_t a) { return a > kAnalogPushed; };
    bool analog = std::any_of(std::begin(p.analog), std::end(p.analog), pressed);
    bool sticks = pushed(p.lx) || pushed(p.ly) || pushed(p.rx) || pushed(p.ry);
    if (p.buttons || analog || sticks) g_keyboard = false;
}

void SetMenu(bool menu) { g_menu = menu; }
bool KeyboardActive() { return g_keyboard; }
void TapConfirm() { g_confirm_until = NowMs() + kTapMs; }

void SetTextEntry(bool on) {
    if (g_text.exchange(on) == on) return;
    std::lock_guard lock(g_chars_lock);
    g_chars.clear();
}

void OnChar(unsigned c) {
    if (!g_text || !Active() || c > 0xFFFF) return;
    std::lock_guard lock(g_chars_lock);
    if (g_chars.size() < kMaxChars) g_chars += char16_t(c);
}

std::u16string TakeChars() {
    std::lock_guard lock(g_chars_lock);
    return std::exchange(g_chars, {});
}

int TakeWheel() {
    int w = g_wheel.load();
    while (w && !g_wheel.compare_exchange_weak(w, w > 0 ? w - 1 : w + 1)) {}
    return w > 0 ? 1 : w < 0 ? -1 : 0;
}

const Action* Actions(size_t* count) {
    static const auto list = [] {
        std::array<Action, kCount> a{};
        for (size_t i = 0; i < kCount; i++) a[i] = kActions[i].action;
        return a;
    }();
    *count = kCount;
    return list.data();
}

std::string Binding(size_t action) {
    std::lock_guard lock(g_bind_lock);
    return action < kCount ? Join(g_binds[action]) : std::string();
}

void Bind(size_t action, int vk, int slot) {
    if (action >= kCount || slot < 0 || slot > 1 || (vk && !NameOf(vk))) return;
    if (kActions[action].button == kHostKey && Mouse(vk)) return;
    {
        std::lock_guard lock(g_bind_lock);
        std::vector<int>& keys = g_binds[action];
        int slots[2] = {0, 0};
        for (size_t i = 0; i < keys.size() && i < 2; i++) slots[i] = keys[i];
        if (vk && slots[1 - slot] == vk) slots[1 - slot] = 0;
        slots[slot] = vk;
        keys.clear();
        for (int k : slots) {
            if (k) keys.push_back(k);
        }
        game::Opt().keys[kActions[action].action.id] = Join(keys);
    }
    app::Save();
}

void Unbind(size_t action) {
    if (action >= kCount) return;
    {
        std::lock_guard lock(g_bind_lock);
        g_binds[action].clear();
        game::Opt().keys[kActions[action].action.id].clear();
    }
    app::Save();
}

bool TakeDeletePress() {
    static bool held = false;
    bool down = Down(VK_DELETE);
    bool fresh = down && !held;
    held = down;
    return fresh && Active();
}

void ResetBindings() {
    game::Opt().keys.clear();
    LoadBindings();
    app::Save();
}

void LoadBindings() {
    std::lock_guard lock(g_bind_lock);
    const auto& keys = game::Opt().keys;
    for (size_t i = 0; i < kCount; i++) {
        auto it = keys.find(kActions[i].action.id);
        g_binds[i] = Parse(it != keys.end() ? it->second : kActions[i].action.defaults);
    }
}

std::string ButtonKey(int button) {
    std::lock_guard lock(g_bind_lock);
    for (size_t i = 0; i < kCount; i++) {
        if (kActions[i].button != button || g_binds[i].empty()) continue;
        std::string name = g_binds[i][0] == VK_ESCAPE ? "Esc" : NameOf(g_binds[i][0]);
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return char(std::toupper(c)); });
        return name;
    }
    return {};
}

int PressedKey() {
    g_capture_ms = NowMs();
    for (const Name& n : kNames)
        if (Down(n.vk)) return n.vk;
    return 0;
}

bool HudKey(int vk) { return HostKeyBound(kHudAction, vk); }

}  // namespace input

// ============================================================================
// MC3D Remix Mobile Touch Controls + SVG Skin Atlas (LunaSVG)
// ============================================================================
namespace touch {

namespace {

using input::NowMs;

enum class Role : uint8_t { None, Button, RadioSwipe, GarageSwipe, Drag, Pinch };

// Controller press targets
enum : int {
    kSteerLeft = -10,
    kSteerRight = -11,
    kAnalogA = 0,
    kAnalogB = 1,
    kAnalogX = 2,
    kAnalogY = 3,
    kAnalogBlack = 4,
    kAnalogWhite = 5,
    kAnalogLT = 6,
    kAnalogRT = 7,
};
constexpr int kBit = 8;  // 8=DUp, 9=DDown, 10=DLeft, 11=DRight, 12=Start, 13=Back, 14=L3, 15=R3

enum class SvgIcon : uint8_t {
    SteerLeft = 0,
    SteerRight,
    Gas,
    Brake,
    Handbrake,
    Nitro,
    Turbo,
    Special,
    SpecialZone,
    SpecialAgro,
    SpecialRoar,
    Start,
    Camera,
    Map,
    FlashLights,
    LookBack,
    DpadUp,
    DpadDown,
    DpadLeft,
    DpadRight,
    Ok,
    Back,
    Count,
};

constexpr size_t kSvgIconCount = static_cast<size_t>(SvgIcon::Count);

struct SvgSpec {
    const char* file;     // basename without .svg
    uint32_t cell_w;      // rasterized width in atlas
    uint32_t cell_h;      // rasterized height in atlas
    float aspect_w_over_h; // drawn W/H aspect ratio
};

// Rectangular aspect ratios matching user's mobile UX spec:
//   steer-left / steer-right / gas : 1:2 (W:H -> 0.50)
//   brake                          : 2:3 (W:H -> 0.667)
//   all other icons                : 1:1 (W:H -> 1.00)
constexpr SvgSpec kSvgSpecs[kSvgIconCount] = {
    {"steer-left",   128, 256, 0.50f},
    {"steer-right",  128, 256, 0.50f},
    {"gas",          128, 256, 0.50f},
    {"brake",        170, 256, 2.0f / 3.0f},
    {"handbrake",    192, 192, 1.0f},
    {"nitro",        192, 192, 1.0f},
    {"turbo",        192, 192, 1.0f},
    {"special",      192, 192, 1.0f},
    {"special-zone", 192, 192, 1.0f},
    {"special-agro", 192, 192, 1.0f},
    {"special-roar", 192, 192, 1.0f},
    {"start",        192, 192, 1.0f},
    {"camera",       192, 192, 1.0f},
    {"map",          192, 192, 1.0f},
    {"flash-lights", 192, 192, 1.0f},
    {"look-back",    192, 192, 1.0f},
    {"dpad-up",      192, 192, 1.0f},
    {"dpad-down",    192, 192, 1.0f},
    {"dpad-left",    192, 192, 1.0f},
    {"dpad-right",   192, 192, 1.0f},
    {"ok",           192, 192, 1.0f},
    {"back",         192, 192, 1.0f},
};

enum Skin : uint32_t { kOutline = 0, kFilledWhite = 1, kFilledDark = 2, kSkins = 3 };
constexpr const char* kSkinDirs[kSkins] = {"outline", "filled-white", "filled-dark"};
constexpr const char* kSkinNames[kSkins] = {"Outline", "Filled White", "Filled Dark"};

enum class When : uint8_t {
    Play,           // Always in active driving gameplay
    PlayNos,        // Dynamic: gameplay only when NOS purchased (max_nos > 0)
    PlayTurbo,      // Dynamic: gameplay only when slipstream filled AND speed >= 10 km/h
    PlaySpecial,    // Dynamic: gameplay only when special ability ready
    MainMenuOnly,   // Main Menu / Pause Menu only (hidden in Garage)
    MenusAndGarage, // Main Menu / Pause Menu AND Garage
};

struct Button {
    const char* id;
    const char* label;
    SvgIcon icon;
    int press;
    When when;
    float w_over_h;  // width / height ratio (0.50 = 1:2, 0.667 = 2:3, 1.0 = 1:1)
    bool keep = false;
    bool steer_pair = false;  // supports seamless left<->right thumb slide
};

constexpr Button kButtons[] = {
    // Gameplay driving controls
    {"steer_left",  "Steer Left",       SvgIcon::SteerLeft,   kSteerLeft,    When::Play,           0.50f,       true,  true},
    {"steer_right", "Steer Right",      SvgIcon::SteerRight,  kSteerRight,   When::Play,           0.50f,       true,  true},
    {"gas",         "Gas",              SvgIcon::Gas,         kAnalogRT,     When::Play,           0.50f,       true,  false},
    {"brake",       "Brake",            SvgIcon::Brake,       kAnalogLT,     When::Play,           2.0f / 3.0f, true,  false},
    {"handbrake",   "Handbrake",        SvgIcon::Handbrake,   kAnalogA,      When::Play,           1.0f,        false, false},
    {"nitro",       "Nitro",            SvgIcon::Nitro,       kAnalogX,      When::PlayNos,        1.0f,        false, false},
    {"turbo",       "Slipstream Turbo", SvgIcon::Turbo,       kAnalogX,      When::PlayTurbo,      1.0f,        false, false},
    {"special",     "Special Ability",  SvgIcon::Special,     kBit + 6,      When::PlaySpecial,    1.0f,        false, false},
    {"pause",       "Pause",            SvgIcon::Start,       kBit + 4,      When::Play,           1.0f,        true,  false},
    {"camera",      "Camera",           SvgIcon::Camera,      kAnalogBlack,  When::Play,           1.0f,        false, false},
    {"map",         "Map",              SvgIcon::Map,         kBit + 5,      When::Play,           1.0f,        false, false},
    {"flash",       "Flash Lights",     SvgIcon::FlashLights, kAnalogWhite,  When::Play,           1.0f,        false, false},
    {"look_back",   "Look Back",        SvgIcon::LookBack,    kAnalogY,      When::Play,           1.0f,        false, false},

    // Menu & Garage minimal navigation controls
    {"dpad_up",     "D-Pad Up",         SvgIcon::DpadUp,      kBit + 0,      When::MainMenuOnly,   1.0f,        false, false},
    {"dpad_down",   "D-Pad Down",       SvgIcon::DpadDown,    kBit + 1,      When::MainMenuOnly,   1.0f,        false, false},
    {"dpad_left",   "D-Pad Left",       SvgIcon::DpadLeft,    kBit + 2,      When::MenusAndGarage, 1.0f,        true,  false},
    {"dpad_right",  "D-Pad Right",      SvgIcon::DpadRight,   kBit + 3,      When::MenusAndGarage, 1.0f,        true,  false},
    {"ok",          "OK",               SvgIcon::Ok,          kAnalogA,      When::MenusAndGarage, 1.0f,        true,  false},
    {"back",        "Back",             SvgIcon::Back,        kAnalogB,      When::MenusAndGarage, 1.0f,        true,  false},
};

constexpr size_t kButtonCount = std::size(kButtons);
constexpr size_t kIdxSteerLeft = 0;
constexpr size_t kIdxSteerRight = 1;

// Centre (x, y) in screen shares; r = half-height in screen heights (drawn H = 2*r*scale*H, W = H * w_over_h)
struct Place {
    float x, y, r;
    bool hidden = false;
};
using Layout = std::array<Place, kButtonCount>;

constexpr Layout kDefaultLayout = {{
    // Gameplay: tall 1:2 steering on bottom-left, tall 1:2 gas & 2:3 brake on bottom-right
    {0.068f, 0.720f, 0.175f, false},  // steer_left  (1:2)
    {0.184f, 0.720f, 0.175f, false},  // steer_right (1:2)
    {0.932f, 0.720f, 0.185f, false},  // gas         (1:2)
    {0.812f, 0.765f, 0.140f, false},  // brake       (2:3)
    {0.812f, 0.485f, 0.066f, false},  // handbrake   (1:1)
    {0.932f, 0.395f, 0.072f, false},  // nitro       (1:1, dynamic)
    {0.700f, 0.560f, 0.074f, false},  // turbo       (1:1, dynamic)
    {0.700f, 0.765f, 0.072f, false},  // special     (1:1, dynamic)
    {0.500f, 0.082f, 0.048f, false},  // pause       (top center)
    {0.422f, 0.082f, 0.046f, false},  // camera      (top left of pause)
    {0.578f, 0.082f, 0.046f, false},  // map         (top right of pause)
    {0.932f, 0.190f, 0.056f, false},  // flash       (upper right)
    {0.344f, 0.082f, 0.046f, true},   // look_back   (hidden by default for minimal HUD; unhideable in editor)

    // Menus & Garage: minimal D-Pad + OK + Back
    {0.122f, 0.555f, 0.058f, false},  // dpad_up
    {0.122f, 0.805f, 0.058f, false},  // dpad_down
    {0.056f, 0.680f, 0.058f, false},  // dpad_left
    {0.188f, 0.680f, 0.058f, false},  // dpad_right
    {0.916f, 0.715f, 0.076f, false},  // ok
    {0.798f, 0.800f, 0.066f, false},  // back
}};

constexpr uint64_t kShownMs = 8000;
constexpr uint64_t kBackTapMs = 120;
constexpr uint64_t kPulseMs = 105;
constexpr uint64_t kToastMs = 800;

struct Finger {
    SDL_FingerID id = 0;
    Role role = Role::None;
    size_t button = 0;
    float x0 = 0, y0 = 0;
    float x = 0, y = 0;
    uint64_t down_ms = 0;
    bool swipe_fired = false;
    float notch_y = 0;
};

std::mutex g_lock;
std::vector<Finger> g_fingers;
bool g_pressed[kButtonCount] = {};
std::atomic<uint64_t> g_touch_ms{0};
std::atomic<uint64_t> g_back_until{0};

// Radio swipe & Garage vertical swipe short pulses
uint64_t g_pulse_until[4] = {};  // 0=DUp, 1=DDown, 2=DLeft (Radio Prev), 3=DRight (Radio Next)
uint64_t g_radio_toast_until = 0;
int g_radio_toast_dir = 0;       // -1 = prev, +1 = next

Layout g_layout = kDefaultLayout;
uint32_t g_skin = kOutline;
float g_opacity = 0.85f;
float g_scale = 1.0f;
bool g_loaded = false;

std::atomic<bool> g_editing{false};
Layout g_before = kDefaultLayout;
float g_before_opacity = 0.85f;
int g_selected = -1;

// Atlas UV rects for each (skin, icon)
struct AtlasUv {
    float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
    bool valid = false;
};
AtlasUv g_atlas_uv[kSkins][kSvgIconCount]{};
ImTextureID g_skin_texture = nullptr;

float Width() {
    int w, h;
    host::WindowPixels(w, h);
    return float(std::max(w, 1));
}
float Height() {
    int w, h;
    host::WindowPixels(w, h);
    return float(std::max(h, 1));
}

std::filesystem::path LayoutFile() { return app::ExeDir() / "layout.toml"; }

void LoadLayout() {
    g_loaded = true;
    g_layout = kDefaultLayout;
    try {
        toml::table t = toml::parse_file(LayoutFile().string());
        g_opacity = std::clamp(float(t["opacity"].value_or(double(g_opacity))), 0.2f, 1.0f);
        g_scale = std::clamp(float(t["scale"].value_or(double(g_scale))), 0.5f, 1.5f);
        std::string skin = t["skin"].value_or(std::string(kSkinDirs[g_skin]));
        for (uint32_t i = 0; i < kSkins; i++) {
            if (skin == kSkinDirs[i]) g_skin = i;
        }
        // Only load per-button custom placement when saved with MC3D layout version >= 2
        if (t["layout_version"].value_or(0) >= 2) {
            for (size_t i = 0; i < kButtonCount; i++) {
                const toml::table* p = t["controls"][kButtons[i].id].as_table();
                if (!p) continue;
                Place& pl = g_layout[i];
                pl.x = std::clamp(float((*p)["x"].value_or(double(pl.x))), 0.02f, 0.98f);
                pl.y = std::clamp(float((*p)["y"].value_or(double(pl.y))), 0.04f, 0.96f);
                pl.r = std::clamp(float((*p)["r"].value_or(double(pl.r))), 0.03f, 0.28f);
                pl.hidden = (*p)["hidden"].value_or(pl.hidden) && !kButtons[i].keep;
            }
        }
    } catch (...) {
    }
}

void SaveLayout() {
    toml::table controls;
    for (size_t i = 0; i < kButtonCount; i++) {
        const Place& p = g_layout[i];
        controls.insert(kButtons[i].id,
                        toml::table{{"x", double(p.x)}, {"y", double(p.y)}, {"r", double(p.r)}, {"hidden", p.hidden}});
    }
    toml::table t{
        {"layout_version", 2},
        {"opacity", double(g_opacity)},
        {"scale", double(g_scale)},
        {"skin", kSkinDirs[g_skin]},
        {"controls", controls},
    };
    std::ofstream(LayoutFile()) << t << "\n";
}

void EnsureLayout() {
    if (!g_loaded) LoadLayout();
}

game::UiContext CurrentUiContext() {
    return static_cast<game::UiContext>(game::Telemetry().ui_context.load(std::memory_order_relaxed));
}

bool IsMenuLike(game::UiContext ctx) {
    return ctx == game::UiContext::MainMenu || ctx == game::UiContext::Garage;
}

bool IsGameplayButton(When w) {
    return w == When::Play || w == When::PlayNos || w == When::PlayTurbo || w == When::PlaySpecial;
}

bool ButtonShown(size_t i) {
    if (!g_editing && !game::Opt().touch_controls) return false;
    const Button& b = kButtons[i];
    if (g_editing) return IsGameplayButton(b.when);
    if (g_layout[i].hidden) return false;

    const auto ctx = CurrentUiContext();
    switch (b.when) {
        case When::MainMenuOnly:
            return ctx == game::UiContext::MainMenu;
        case When::MenusAndGarage:
            return ctx == game::UiContext::MainMenu || ctx == game::UiContext::Garage;
        case When::Play:
            return ctx == game::UiContext::Gameplay;
        case When::PlayNos:
            return ctx == game::UiContext::Gameplay &&
                   game::Telemetry().has_nos.load(std::memory_order_relaxed);
        case When::PlayTurbo:
            return ctx == game::UiContext::Gameplay &&
                   game::Telemetry().turbo_ready.load(std::memory_order_relaxed);
        case When::PlaySpecial:
            return ctx == game::UiContext::Gameplay &&
                   game::Telemetry().special_ready.load(std::memory_order_relaxed);
    }
    return false;
}

void ButtonRect(size_t i, float& x0, float& y0, float& x1, float& y1) {
    const float cx = g_layout[i].x * Width();
    const float cy = g_layout[i].y * Height();
    const float half_h = g_layout[i].r * g_scale * Height();
    const float half_w = half_h * kButtons[i].w_over_h;
    x0 = cx - half_w;
    y0 = cy - half_h;
    x1 = cx + half_w;
    y1 = cy + half_h;
}

// Hit test with generous touch slack around rectangular & circular controls
size_t HitButton(float x, float y) {
    for (size_t i = 0; i < kButtonCount; i++) {
        if (!ButtonShown(i)) continue;
        float x0, y0, x1, y1;
        ButtonRect(i, x0, y0, x1, y1);
        const float cx = (x0 + x1) * 0.5f;
        const float cy = (y0 + y1) * 0.5f;
        const float hw = (x1 - x0) * 0.5f;
        const float hh = (y1 - y0) * 0.5f;
        if (kButtons[i].w_over_h < 0.95f) {
            // Rectangular pedal / tall steering button: tight horizontal slack so Left & Right stay distinct
            const float sx = hw * 1.12f;
            const float sy = hh * 1.14f;
            if (std::fabs(x - cx) <= sx && std::fabs(y - cy) <= sy) return i;
        } else {
            const float r = hh * 1.22f;
            if ((x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r) return i;
        }
    }
    return kButtonCount;
}

// Editor top bar
constexpr const char* kBarLabels[] = {"RESET", "-", "+", "HIDE", "CANCEL", "DONE"};
enum { kBarReset, kBarSmaller, kBarBigger, kBarHide, kBarCancel, kBarDone, kBarCount };
constexpr float kBarY = 0.16f, kBarH = 0.088f, kBarW = 0.135f, kBarGap = 0.018f;
constexpr float kBarSizeW = 0.07f;
constexpr float kMinRadius = 0.03f, kMaxRadius = 0.28f;
constexpr float kSizeStep = 1.1f;

float BarWidth(int i) { return i == kBarSmaller || i == kBarBigger ? kBarSizeW : kBarW; }
bool Hideable(int i) { return i >= 0 && size_t(i) < kButtonCount && !kButtons[size_t(i)].keep; }
const char* BarLabel(int i) {
    if (i == kBarHide && Hideable(g_selected) && g_layout[size_t(g_selected)].hidden) return "SHOW";
    return kBarLabels[i];
}

int BarAt(float x, float y, float rect[4] = nullptr, int which = -1) {
    float w = Width(), h = Height();
    float total = (kBarCount - 1) * kBarGap;
    for (int i = 0; i < kBarCount; i++) total += BarWidth(i);
    float left = 0.5f - total / 2;
    for (int i = 0; i < kBarCount; i++) {
        float x0 = left * w, y0 = kBarY * h;
        float x1 = x0 + BarWidth(i) * w, y1 = y0 + kBarH * h;
        left += BarWidth(i) + kBarGap;
        if (i == which && rect) {
            rect[0] = x0; rect[1] = y0; rect[2] = x1; rect[3] = y1;
            return i;
        }
        if (which < 0 && x >= x0 && x <= x1 && y >= y0 && y <= y1) return i;
    }
    return -1;
}

void OpenEditor() {
    g_before = g_layout;
    g_before_opacity = g_opacity;
    g_selected = -1;
    g_editing = true;
}

void CloseEditor(bool keep) {
    if (keep) {
        SaveLayout();
    } else {
        g_layout = g_before;
        g_opacity = g_before_opacity;
    }
    g_selected = -1;
    g_editing = false;
}

void Resize(size_t i, float r) { g_layout[i].r = std::clamp(r, kMinRadius, kMaxRadius); }

constexpr uint64_t kStepMs = 110, kRepeatAfterMs = 420, kRepeatMs = 160;
bool DpadStep(uint64_t held) {
    if (held < kStepMs) return true;
    return held >= kRepeatAfterMs && (held - kRepeatAfterMs) % kRepeatMs < kStepMs / 2;
}
bool IsMenuDpad(size_t i) {
    return (kButtons[i].when == When::MainMenuOnly || kButtons[i].when == When::MenusAndGarage) &&
           kButtons[i].press >= kBit && kButtons[i].press < kBit + 4;
}

void Recount() {
    std::fill(std::begin(g_pressed), std::end(g_pressed), false);
    const uint64_t now = NowMs();
    for (const Finger& f : g_fingers) {
        if (f.role == Role::Button && f.button < kButtonCount) {
            g_pressed[f.button] = !IsMenuDpad(f.button) || DpadStep(now - f.down_ms);
        }
    }
}

void EditDown(Finger& f, float x, float y) {
    int bar = BarAt(x, y);
    switch (bar) {
        case kBarReset:
            g_layout = kDefaultLayout;
            g_selected = -1;
            return;
        case kBarSmaller:
        case kBarBigger:
            if (g_selected >= 0) {
                float r = g_layout[size_t(g_selected)].r;
                Resize(size_t(g_selected), bar == kBarBigger ? r * kSizeStep : r / kSizeStep);
            }
            return;
        case kBarHide:
            if (Hideable(g_selected)) g_layout[size_t(g_selected)].hidden ^= true;
            return;
        case kBarCancel:
            CloseEditor(false);
            return;
        case kBarDone:
            CloseEditor(true);
            return;
        default:
            break;
    }
    for (const Finger& d : g_fingers) {
        if (d.role != Role::Drag) continue;
        f.role = Role::Pinch;
        f.button = d.button;
        f.x0 = std::max(std::hypot(x - d.x, y - d.y), 1.0f);
        f.y0 = g_layout[d.button].r;
        return;
    }
    size_t b = HitButton(x, y);
    if (b >= kButtonCount) {
        g_selected = -1;
        return;
    }
    g_selected = int(b);
    f.role = Role::Drag;
    f.button = b;
    f.x0 = x - g_layout[b].x * Width();
    f.y0 = y - g_layout[b].y * Height();
}

void FingerDown(SDL_FingerID id, float x, float y) {
    Finger f;
    f.id = id;
    f.x0 = f.x = x;
    f.y0 = f.y = y;
    f.down_ms = NowMs();
    if (g_editing) {
        EditDown(f, x, y);
        g_fingers.push_back(f);
        return;
    }
    input::g_touch = true;
    const size_t b = HitButton(x, y);
    if (b < kButtonCount) {
        f.role = Role::Button;
        f.button = b;
    } else {
        const auto ctx = CurrentUiContext();
        const float w = Width();
        const float h = Height();
        if (ctx == game::UiContext::Gameplay && game::Opt().touch_controls &&
            x >= w * 0.25f && x <= w * 0.75f && y >= h * 0.62f) {
            // Bottom-center screen horizontal swipe adjusts radio station / track
            f.role = Role::RadioSwipe;
        } else if (ctx == game::UiContext::Garage && game::Opt().touch_controls) {
            // Empty-screen vertical swipe in Garage pulses D-Pad Up/Down for upgrade lists
            f.role = Role::GarageSwipe;
            f.notch_y = y;
        }
    }
    g_fingers.push_back(f);
    Recount();
}

void FingerMove(SDL_FingerID id, float x, float y) {
    const float w = Width();
    const float h = Height();
    const uint64_t now = NowMs();

    for (Finger& f : g_fingers) {
        if (f.id != id) continue;
        f.x = x;
        f.y = y;
        if (f.role == Role::Drag) {
            Place& p = g_layout[f.button];
            p.x = std::clamp((x - f.x0) / w, 0.02f, 0.98f);
            p.y = std::clamp((y - f.y0) / h, 0.04f, 0.96f);
        }
        if (f.role == Role::Drag || f.role == Role::Pinch) {
            for (const Finger& o : g_fingers) {
                const Finger& pinch = f.role == Role::Pinch ? f : o;
                const Finger& drag = f.role == Role::Pinch ? o : f;
                if (&o == &f || pinch.role != Role::Pinch || drag.role != Role::Drag || pinch.button != drag.button) continue;
                Resize(f.button, pinch.y0 * std::hypot(pinch.x - drag.x, pinch.y - drag.y) / pinch.x0);
            }
        }
        if (f.role == Role::Button && f.button < kButtonCount && kButtons[f.button].steer_pair) {
            // Smooth thumb slide between Steer Left and Steer Right without lifting!
            const size_t hit = HitButton(x, y);
            if (hit == kIdxSteerLeft || hit == kIdxSteerRight) {
                f.button = hit;
            }
        }
        if (f.role == Role::RadioSwipe && !f.swipe_fired) {
            const float dx = x - f.x0;
            const float dy = y - f.y0;
            if (std::fabs(dx) >= w * 0.06f && std::fabs(dx) > std::fabs(dy) * 1.25f) {
                f.swipe_fired = true;
                if (dx > 0.0f) {
                    g_pulse_until[3] = now + kPulseMs;  // D-Pad Right = Next Track
                    g_radio_toast_dir = 1;
                } else {
                    g_pulse_until[2] = now + kPulseMs;  // D-Pad Left = Prev Track
                    g_radio_toast_dir = -1;
                }
                g_radio_toast_until = now + kToastMs;
            }
        }
        if (f.role == Role::GarageSwipe) {
            const float notch = h * 0.065f;
            if (y <= f.notch_y - notch) {
                f.notch_y -= notch;
                g_pulse_until[0] = now + kPulseMs;  // D-Pad Up
            } else if (y >= f.notch_y + notch) {
                f.notch_y += notch;
                g_pulse_until[1] = now + kPulseMs;  // D-Pad Down
            }
        }
    }
    Recount();
}

void FingerUp(SDL_FingerID id) {
    std::erase_if(g_fingers, [id](const Finger& f) { return f.id == id; });
    Recount();
}

bool PadPlugged() {
    int n = 0;
    if (SDL_JoystickID* ids = SDL_GetGamepads(&n)) SDL_free(ids);
    return n > 0;
}

void KeyEvent(const SDL_KeyboardEvent& k) {
    if (k.scancode == SDL_SCANCODE_AC_BACK) {
        if (!k.down || k.repeat) return;
        std::lock_guard lock(g_lock);
        if (g_editing) {
            CloseEditor(false);
        } else {
            g_back_until = NowMs() + kBackTapMs;
        }
        return;
    }
    int vk = input::VkOf(k.scancode);
    if (!vk) return;
    input::g_down[size_t(vk)] = k.down;
    if (!k.down) return;
    input::g_keyboard = true;
    input::g_touch = false;
    if (!k.repeat && input::HudKey(vk)) game::g_hide_hud = !game::g_hide_hud;
    if (vk == VK_BACK) input::OnChar('\b');
    if (vk == VK_RETURN) input::OnChar('\r');
}

// ============================================================================
// Live SVG Loader & Aspect-Preserving Container Expansion for 1:2 / 2:3 Pedals
// ============================================================================

void ReplaceAll(std::string& s, std::string_view from, std::string_view to) {
    if (from.empty()) return;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
}

// Expand stock 512x512 square SVGs for tall 1:2 (steer-left, steer-right, gas) and 2:3 (brake)
// buttons so the outer rounded pill/pedal stretches cleanly while inner icons stay un-distorted 1:1.
// Custom user-supplied SVGs that already use a non-square viewBox are rendered untouched.
void AdaptRectangularSvg(SvgIcon icon, std::string& svg) {
    if (svg.find("viewBox=\"0 0 512 512\"") == std::string::npos) return;

    if (icon == SvgIcon::SteerLeft || icon == SvgIcon::SteerRight) {
        ReplaceAll(svg, "viewBox=\"0 0 512 512\" width=\"512\" height=\"512\"",
                        "viewBox=\"0 0 512 1024\" width=\"512\" height=\"1024\"");
        ReplaceAll(svg, "width=\"512\" height=\"512\"", "width=\"512\" height=\"1024\"");
        ReplaceAll(svg, "x=\"12\" y=\"12\" width=\"104\" height=\"104\"",
                        "x=\"12\" y=\"12\" width=\"104\" height=\"232\"");
        ReplaceAll(svg, "<path ", "<path transform=\"translate(0 64)\" ");
    } else if (icon == SvgIcon::Gas) {
        ReplaceAll(svg, "viewBox=\"0 0 512 512\" width=\"512\" height=\"512\"",
                        "viewBox=\"0 0 512 1024\" width=\"512\" height=\"1024\"");
        ReplaceAll(svg, "width=\"512\" height=\"512\"", "width=\"512\" height=\"1024\"");
        ReplaceAll(svg, "x=\"28\" y=\"10\" width=\"72\" height=\"108\" rx=\"22\"",
                        "x=\"12\" y=\"12\" width=\"104\" height=\"232\" rx=\"26\"");
        ReplaceAll(svg, "<path ", "<path transform=\"translate(0 64)\" ");
        ReplaceAll(svg, "<rect x=\"42\" ", "<rect transform=\"translate(0 64)\" x=\"42\" ");
    } else if (icon == SvgIcon::Brake) {
        ReplaceAll(svg, "viewBox=\"0 0 512 512\" width=\"512\" height=\"512\"",
                        "viewBox=\"0 0 512 768\" width=\"512\" height=\"768\"");
        ReplaceAll(svg, "width=\"512\" height=\"512\"", "width=\"512\" height=\"768\"");
        ReplaceAll(svg, "x=\"12\" y=\"24\" width=\"104\" height=\"80\" rx=\"22\"",
                        "x=\"12\" y=\"12\" width=\"104\" height=\"168\" rx=\"24\"");
        ReplaceAll(svg, "<rect x=\"31\" ", "<rect transform=\"translate(0 32)\" x=\"31\" ");
    }
}

bool ReadSvgFile(const char* skin_dir, const char* icon_name, std::string& out) {
    // 1. User-configurable override on storage: <mc3dr>/skins/<skin>/<name>.svg
    const auto user_path = app::ExeDir() / "skins" / skin_dir / (std::string(icon_name) + ".svg");
    {
        std::ifstream f(user_path, std::ios::binary);
        if (f) {
            out.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            if (!out.empty()) return true;
        }
    }
    // 2. Bundled APK asset: assets/skins/mc3dr/<skin>/<name>.svg
    const std::string asset_path = std::string("skins/mc3dr/") + skin_dir + "/" + icon_name + ".svg";
    size_t sz = 0;
    void* data = SDL_LoadFile(asset_path.c_str(), &sz);
    if (!data || sz == 0) {
        if (data) SDL_free(data);
        return false;
    }
    out.assign(static_cast<const char*>(data), sz);
    SDL_free(data);
    return true;
}

SvgIcon ActiveIconForButton(size_t i) {
    const Button& b = kButtons[i];
    if (b.when == When::PlaySpecial && !g_editing) {
        const int8_t st = game::Telemetry().special_type.load(std::memory_order_relaxed);
        if (st == 0) return SvgIcon::SpecialZone;
        if (st == 1) return SvgIcon::SpecialAgro;
        if (st == 2) return SvgIcon::SpecialRoar;
    }
    return b.icon;
}

ImU32 Fade(int r, int g, int b, int a) {
    return IM_COL32(r, g, b, int(float(a) * g_opacity));
}

void CentredText(ImDrawList* dl, ImVec2 at, float size, ImU32 colour, const char* text) {
    ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0, text);
    dl->AddText(ImGui::GetFont(), size, ImVec2(at.x - ts.x / 2, at.y - ts.y / 2), colour, text);
}

void DrawEditorBar(ImDrawList* dl, float th) {
    float h = Height();
    for (int i = 0; i < kBarCount; i++) {
        float r[4];
        BarAt(0, 0, r, i);
        bool done = i == kBarDone;
        bool size = i == kBarSmaller || i == kBarBigger;
        int alpha = (size && g_selected < 0) || (i == kBarHide && !Hideable(g_selected)) ? 90 : 240;
        dl->AddRectFilled(ImVec2(r[0], r[1]), ImVec2(r[2], r[3]),
                          done ? IM_COL32(150, 115, 45, 235) : IM_COL32(20, 18, 14, 225), h * 0.015f);
        dl->AddRect(ImVec2(r[0], r[1]), ImVec2(r[2], r[3]), IM_COL32(220, 180, 100, alpha), h * 0.015f, 0, th * 0.7f);
        CentredText(dl, ImVec2((r[0] + r[2]) / 2, (r[1] + r[3]) / 2), h * (size ? 0.056f : 0.038f),
                    IM_COL32(255, 255, 255, alpha), BarLabel(i));
    }
    CentredText(dl, ImVec2(Width() / 2, (kBarY + kBarH + 0.045f) * h), h * 0.033f, IM_COL32(255, 255, 255, 225),
                "DRAG TO MOVE  |  PINCH OR -/+ TO RESIZE  |  TAP THEN HIDE/SHOW");
}

}  // namespace

bool OnEvent(const SDL_Event& e) {
    switch (e.type) {
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
            KeyEvent(e.key);
            return app::MenuOpen() ? false : true;
        case SDL_EVENT_TEXT_INPUT:
            for (const char* c = e.text.text; c && *c; c++)
                if (uint8_t(*c) < 0x80 && *c >= ' ') input::OnChar(unsigned(*c));
            return !app::MenuOpen();
        case SDL_EVENT_MOUSE_MOTION:
            if (e.motion.which == SDL_TOUCH_MOUSEID) return true;
            input::g_keyboard = true;
            input::g_touch = false;
            if (input::Active() && input::Play()) {
                AddLook(e.motion.xrel, e.motion.yrel);
            }
            return !app::MenuOpen();
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (e.button.which == SDL_TOUCH_MOUSEID) return true;
            if (int vk = input::VkOfButton(e.button.button)) input::g_down[size_t(vk)] = e.button.down;
            input::g_keyboard = true;
            input::g_touch = false;
            return !app::MenuOpen();
        case SDL_EVENT_MOUSE_WHEEL:
            if (e.wheel.y != 0) {
                int notch = e.wheel.y > 0 ? 1 : -1;
                input::g_wheel_ms[notch < 0] = NowMs();
                if (input::Active() && !input::Play() && !input::KeyCaptureUp()) input::g_wheel += notch;
            }
            return !app::MenuOpen();
        case SDL_EVENT_FINGER_DOWN:
        case SDL_EVENT_FINGER_MOTION:
        case SDL_EVENT_FINGER_UP:
        case SDL_EVENT_FINGER_CANCELED: {
            g_touch_ms = NowMs();
            float x = e.tfinger.x * Width(), y = e.tfinger.y * Height();
            std::lock_guard lock(g_lock);
            EnsureLayout();
            if (e.type == SDL_EVENT_FINGER_DOWN) {
                FingerDown(e.tfinger.fingerID, x, y);
                return true;
            }
            bool ours = std::any_of(g_fingers.begin(), g_fingers.end(),
                                    [&](const Finger& f) { return f.id == e.tfinger.fingerID; });
            if (!ours) return false;
            if (e.type == SDL_EVENT_FINGER_MOTION) FingerMove(e.tfinger.fingerID, x, y);
            else FingerUp(e.tfinger.fingerID);
            return true;
        }
        default:
            return false;
    }
}

void Apply(int port, input::Pad& pad) {
    if (port != 0 || g_editing) return;
    std::lock_guard lock(g_lock);
    Recount();
    const uint64_t now = NowMs();
    bool used = false;
    int steer = 0;

    for (size_t i = 0; i < kButtonCount; i++) {
        if (!g_pressed[i]) continue;
        const int p = kButtons[i].press;
        used = true;
        if (p == kSteerLeft) {
            steer -= 1;
        } else if (p == kSteerRight) {
            steer += 1;
        } else if (p >= 0 && p < kBit) {
            pad.analog[p] = 0xFF;
        } else if (p >= kBit) {
            pad.buttons |= uint16_t(1u << (p - kBit));
        }
    }

    if (steer != 0) {
        pad.lx = (steer < 0) ? int16_t(-32767) : int16_t(32767);
    }

    // Radio swipe (D-Pad Left/Right) & Garage vertical swipe (D-Pad Up/Down) pulses
    for (int bit = 0; bit < 4; ++bit) {
        if (now < g_pulse_until[bit]) {
            pad.buttons |= uint16_t(1u << bit);
            used = true;
        }
    }

    if (now < g_back_until) {
        used = true;
        if (IsMenuLike(CurrentUiContext())) pad.analog[kAnalogB] = 0xFF;
        else pad.buttons |= uint16_t(1u << 4);  // Start (Pause)
    }

    if (used) pad.connected = true;
}

bool Shown() { return !PadPlugged() || NowMs() - g_touch_ms < kShownMs; }

void Draw() {
    std::lock_guard lock(g_lock);
    EnsureLayout();
    const bool editing = g_editing;
    if (!editing && (!game::Opt().touch_controls || !Shown())) return;

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const float w = Width();
    const float h = Height();
    const float th = std::max(2.0f, h / 360.0f);
    if (editing) dl->AddRectFilled(ImVec2(0, 0), ImVec2(w, h), IM_COL32(0, 0, 0, 105));

    const uint32_t skin = std::min<uint32_t>(g_skin, kSkins - 1);

    for (size_t i = 0; i < kButtonCount; i++) {
        if (!ButtonShown(i)) continue;
        float x0, y0, x1, y1;
        ButtonRect(i, x0, y0, x1, y1);
        const float cx = (x0 + x1) * 0.5f;
        const float cy = (y0 + y1) * 0.5f;
        const float bw = x1 - x0;
        const float bh = y1 - y0;

        const bool hidden = g_layout[i].hidden;
        const bool on = !editing && g_pressed[i];
        const int fade = hidden ? 40 : 100;
        const float press_scale = on ? 0.94f : 1.0f;

        const float dx0 = cx - bw * 0.5f * press_scale;
        const float dy0 = cy - bh * 0.5f * press_scale;
        const float dx1 = cx + bw * 0.5f * press_scale;
        const float dy1 = cy + bh * 0.5f * press_scale;

        const SvgIcon icon = ActiveIconForButton(i);
        const AtlasUv& uv = g_atlas_uv[skin][static_cast<size_t>(icon)];

        if (g_skin_texture && uv.valid) {
            ImU32 tint;
            if (kButtons[i].when == When::PlayTurbo && !editing) {
                // Highlight ready Slipstream Turbo with warm electric cyan/gold tint
                tint = on ? Fade(255, 240, 180, 255) : Fade(120, 235, 255, 245);
            } else {
                tint = Fade(255, 255, 255, on ? 255 : (225 * fade / 100));
            }
            dl->AddImage(g_skin_texture, ImVec2(dx0, dy0), ImVec2(dx1, dy1),
                         ImVec2(uv.u0, uv.v0), ImVec2(uv.u1, uv.v1), tint);
        } else {
            // Crisp fallback vector pill if SVG atlas unavailable
            const float rounding = std::min(bw, bh) * 0.25f;
            dl->AddRectFilled(ImVec2(dx0, dy0), ImVec2(dx1, dy1),
                              on ? Fade(255, 255, 255, 95) : Fade(15, 15, 20, 120 * fade / 100), rounding);
            dl->AddRect(ImVec2(dx0, dy0), ImVec2(dx1, dy1),
                        Fade(255, 255, 255, 190 * fade / 100), rounding, 0, th);
            CentredText(dl, ImVec2(cx, cy), std::min(bw, bh) * 0.24f,
                        Fade(255, 255, 255, 220 * fade / 100), kButtons[i].label);
        }

        if (editing) {
            if (int(i) == g_selected) {
                dl->AddRect(ImVec2(x0 - th * 2, y0 - th * 2), ImVec2(x1 + th * 2, y1 + th * 2),
                            IM_COL32(255, 200, 60, 255), std::min(bw, bh) * 0.22f, 0, th * 1.5f);
            }
            if (hidden) {
                dl->AddLine(ImVec2(x0 + bw * 0.2f, y0 + bh * 0.2f), ImVec2(x1 - bw * 0.2f, y1 - bh * 0.2f),
                            IM_COL32(255, 80, 60, 220), th * 1.2f);
                dl->AddLine(ImVec2(x0 + bw * 0.2f, y1 - bh * 0.2f), ImVec2(x1 - bw * 0.2f, y0 + bh * 0.2f),
                            IM_COL32(255, 80, 60, 220), th * 1.2f);
            }
            CentredText(dl, ImVec2(cx, y1 + h * 0.020f), h * 0.022f, IM_COL32(255, 255, 255, 210), kButtons[i].label);
        }
    }

    // Subtle fading Radio swipe toast at bottom-center during gameplay
    const uint64_t now = NowMs();
    if (!editing && CurrentUiContext() == game::UiContext::Gameplay && now < g_radio_toast_until) {
        const float remain = float(g_radio_toast_until - now) / float(kToastMs);
        const int alpha = int(std::clamp(remain * 1.5f, 0.0f, 1.0f) * 220.0f);
        const char* msg = (g_radio_toast_dir > 0) ? "RADIO  >" : "<  RADIO";
        const float ty = h * 0.88f;
        const float fs = h * 0.036f;
        const ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(fs, FLT_MAX, 0, msg);
        dl->AddRectFilled(ImVec2(w * 0.5f - ts.x * 0.7f, ty - ts.y * 0.7f),
                          ImVec2(w * 0.5f + ts.x * 0.7f, ty + ts.y * 0.7f),
                          IM_COL32(15, 15, 20, alpha * 2 / 3), h * 0.018f);
        CentredText(dl, ImVec2(w * 0.5f, ty), fs, IM_COL32(255, 255, 255, alpha), msg);
    }

    if (editing) {
        DrawEditorBar(dl, th);
    }
}

// Rasterize all 3 SVG skins (outline, filled-white, filled-dark) into one Vulkan RGBA atlas using LunaSVG
bool LoadSkin(std::vector<uint8_t>& rgba, uint32_t& out_w, uint32_t& out_h) {
    constexpr uint32_t kAtlasW = 2048;
    constexpr uint32_t kAtlasH = 2048;
    constexpr uint32_t kPad = 4;

    rgba.assign(size_t(kAtlasW) * kAtlasH * 4u, 0u);
    uint32_t cur_x = kPad;
    uint32_t cur_y = kPad;
    uint32_t row_h = 0;
    int loaded_count = 0;

    for (uint32_t s = 0; s < kSkins; ++s) {
        for (size_t i = 0; i < kSvgIconCount; ++i) {
            const auto icon = static_cast<SvgIcon>(i);
            const SvgSpec& spec = kSvgSpecs[i];
            g_atlas_uv[s][i] = AtlasUv{};

            std::string svg;
            if (!ReadSvgFile(kSkinDirs[s], spec.file, svg)) {
                LOGW("touch: missing svg skins/mc3dr/%s/%s.svg", kSkinDirs[s], spec.file);
                continue;
            }
            AdaptRectangularSvg(icon, svg);

            auto doc = lunasvg::Document::loadFromData(svg.data(), svg.size());
            if (!doc) {
                LOGW("touch: failed to parse svg %s/%s.svg", kSkinDirs[s], spec.file);
                continue;
            }
            auto bmp = doc->renderToBitmap(spec.cell_w, spec.cell_h, 0x00000000u);
            if (!bmp.valid()) continue;
            bmp.convertToRGBA();

            if (cur_x + spec.cell_w + kPad > kAtlasW) {
                cur_x = kPad;
                cur_y += row_h + kPad;
                row_h = 0;
            }
            if (cur_y + spec.cell_h + kPad > kAtlasH) {
                LOGE("touch: SVG atlas overflow at %s/%s", kSkinDirs[s], spec.file);
                break;
            }

            const uint8_t* src = bmp.data();
            const uint32_t src_stride = bmp.stride();
            for (uint32_t y = 0; y < spec.cell_h; ++y) {
                uint8_t* dst = &rgba[(size_t(cur_y + y) * kAtlasW + cur_x) * 4u];
                std::memcpy(dst, src + size_t(y) * src_stride, size_t(spec.cell_w) * 4u);
            }

            g_atlas_uv[s][i] = AtlasUv{
                float(cur_x) / float(kAtlasW),
                float(cur_y) / float(kAtlasH),
                float(cur_x + spec.cell_w) / float(kAtlasW),
                float(cur_y + spec.cell_h) / float(kAtlasH),
                true,
            };
            cur_x += spec.cell_w + kPad;
            row_h = std::max(row_h, spec.cell_h);
            ++loaded_count;
        }
    }

    out_w = kAtlasW;
    out_h = kAtlasH;
    LOGI("[OK] touch: rasterized %d SVG button icons across %u skins into %ux%u atlas",
         loaded_count, kSkins, out_w, out_h);
    return loaded_count > 0;
}

void SetSkinTexture(uint64_t texture) {
    g_skin_texture = reinterpret_cast<ImTextureID>(uintptr_t(texture));
}

// ============================================================================
// Options API (called from src/common/menu.cpp Touchscreen Options)
// ============================================================================
namespace {
uint32_t Percent(float f) { return uint32_t(std::lround(f * 100.0f)); }
}  // namespace

const char* LayoutName() { return "DEFAULT"; }
void StepLayout(int) {}

uint32_t ControlsOpacity() {
    std::lock_guard lock(g_lock);
    EnsureLayout();
    return Percent(g_opacity);
}

void StepOpacity(int step) {
    std::lock_guard lock(g_lock);
    EnsureLayout();
    g_opacity = float(std::clamp(int(Percent(g_opacity)) + step * 5, 20, 100)) / 100.0f;
    SaveLayout();
}

uint32_t ControlsSize() {
    std::lock_guard lock(g_lock);
    EnsureLayout();
    return Percent(g_scale);
}

void StepSize(int step) {
    std::lock_guard lock(g_lock);
    EnsureLayout();
    g_scale = float(std::clamp(int(Percent(g_scale)) + step * 5, 50, 150)) / 100.0f;
    SaveLayout();
}

const char* SkinName() {
    std::lock_guard lock(g_lock);
    EnsureLayout();
    return kSkinNames[std::min<uint32_t>(g_skin, kSkins - 1)];
}

void StepSkin(int step) {
    std::lock_guard lock(g_lock);
    EnsureLayout();
    g_skin = (g_skin + (step > 0 ? 1 : kSkins - 1)) % kSkins;
    SaveLayout();
}

const char* StickModeName() { return "BUTTONS"; }
void StepStickMode(int) {}

void EditLayout() {
    std::lock_guard lock(g_lock);
    EnsureLayout();
    g_fingers.clear();
    Recount();
    OpenEditor();
}

void ResetLayout() {
    std::lock_guard lock(g_lock);
    EnsureLayout();
    g_layout = kDefaultLayout;
    g_opacity = 0.85f;
    g_scale = 1.0f;
    SaveLayout();
}

void AddLook(float dx, float dy) {
    static float rx = 0, ry = 0;
    rx += dx;
    ry += dy;
    int ix = int(rx), iy = int(ry);
    rx -= float(ix);
    ry -= float(iy);
    input::g_dx += ix;
    input::g_dy += iy;
}

void PadUsed() { input::g_touch = false; }
bool LookActive() { return !g_editing && input::Active() && input::Play() && !IsMenuLike(CurrentUiContext()); }

bool Firing() {
    std::lock_guard lock(g_lock);
    for (size_t i = 0; i < kButtonCount; i++) {
        if (g_pressed[i] && kButtons[i].press == kAnalogRT) return true;
    }
    return false;
}

void Pump() {
    gyro::Pump();
    SDL_Window* window = host::Window();
    bool capture = input::g_keyboard && !input::g_touch && LookActive();
    if (window && capture != SDL_GetWindowRelativeMouseMode(window)) SDL_SetWindowRelativeMouseMode(window, capture);
    bool want = input::g_text && input::Active();
    if (want == input::g_text_shown) return;
    input::g_text_shown = want;
    if (want) SDL_StartTextInput(host::Window());
    else SDL_StopTextInput(host::Window());
}

}  // namespace touch

namespace input {

bool CursorShown() { return false; }
bool Cursor(float&, float&, bool&) { return false; }

bool LeftButton() {
    if (!Active() || g_settle) return false;
    if (Down(VK_LBUTTON)) return true;
    std::lock_guard lock(touch::g_lock);
    return std::any_of(touch::g_fingers.begin(), touch::g_fingers.end(),
                       [](const touch::Finger& f) { return f.role != touch::Role::Button; });
}

bool TouchActive() { return g_touch; }

std::string TouchName(int button) {
    std::lock_guard lock(touch::g_lock);
    for (size_t i = 0; i < touch::kButtonCount; i++) {
        const touch::Button& b = touch::kButtons[i];
        if (b.press != button || touch::g_layout[i].hidden) continue;
        std::string name = b.label;
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return char(std::toupper(c)); });
        return name;
    }
    return {};
}

}  // namespace input
