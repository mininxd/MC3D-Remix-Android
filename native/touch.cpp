// Android native Vulkan 2D touch controls, SVG skin rasterizer (LunaSVG), dynamic 200ms auto-fade,
// FMV hold-anywhere circular skip animation (start.svg), and custom zero-ImGui layout editor.
#include "common/mouse_keyboard.h"

#include <SDL3/SDL.h>
#include <lunasvg.h>
#include <toml++/toml.hpp>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
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
// MC3D Remix Native Vulkan 2D Touch Controls + Custom Zero-ImGui Editor
// ============================================================================
namespace touch {

namespace {

using input::NowMs;

enum class Role : uint8_t { None, Button, AnalogStick, RadioSwipe, GarageOrbit, FmvHold, Drag, Pinch, ToolbarDrag };

// Controller press targets
enum : int {
    kSteerLeft = -10,
    kSteerRight = -11,
    kBurnout = -12,     // Simultaneous Gas + Handbrake (synced with remapped bindings)
    kSteerStick = -13,  // Progressive NFSU2-style analog steering thumbstick
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
    WeightTransfer,
    Burnout,
    Nitro,
    Turbo,
    Special,
    SpecialZone,
    SpecialAgro,
    SpecialRoar,
    Start,
    Pause,
    Camera,
    Map,
    FlashLights,
    LookBack,
    DpadUp,
    DpadDown,
    DpadLeft,
    DpadRight,
    DpadUpGarage,
    DpadDownGarage,
    DpadLeftGarage,
    DpadRightGarage,
    Ok,
    Back,
    Count,
};

constexpr size_t kSvgIconCount = static_cast<size_t>(SvgIcon::Count);

struct SvgSpec {
    const char* file;
    uint32_t cell_w;
    uint32_t cell_h;
};

constexpr SvgSpec kSvgSpecs[kSvgIconCount] = {
    {"steer-left",      128, 256},
    {"steer-right",     128, 256},
    {"gas",             128, 256},
    {"brake",           170, 256},
    {"handbrake",       192, 192},
    {"weight-transfer", 192, 192},
    {"burnout",         192, 192},
    {"nitro",           192, 192},
    {"turbo",           192, 192},
    {"special",         192, 192},
    {"special-zone",    192, 192},
    {"special-agro",    192, 192},
    {"special-roar",    192, 192},
    {"start",           192, 192},
    {"pause",           192, 192},
    {"camera",          192, 192},
    {"map",             192, 192},
    {"flash-lights",    192, 192},
    {"look-back",       192, 192},
    {"dpad-up",         192, 192},
    {"dpad-down",       192, 192},
    {"dpad-left",       192, 192},
    {"dpad-right",      192, 192},
    {"dpad-up",         256, 128},
    {"dpad-down",       256, 128},
    {"dpad-left",       128, 256},
    {"dpad-right",      128, 256},
    {"ok",              284, 120},
    {"back",            284, 120},
};

enum Skin : uint32_t { kOutline = 0, kFilledWhite = 1, kFilledDark = 2, kSkins = 3 };
constexpr const char* kSkinDirs[kSkins] = {"outline", "filled-white", "filled-dark"};
constexpr const char* kSkinNames[kSkins] = {"Outline", "Filled White", "Filled Dark"};

enum class When : uint8_t {
    Play,
    PlaySteerBtn,
    PlayStick,
    PlayNos,
    PlayTurbo,
    PlaySpecial,
    MainMenuOnly,
    MenusAndGarage,
};

struct Button {
    const char* id;
    const char* label;
    SvgIcon icon;
    int press;
    game::GameAction action;  // GameAction::Count = fixed menu control, otherwise synced with MC3D remap
    When when;
    float w_over_h;  // 0.50 = 1:2, 0.667 = 2:3, 1.0 = 1:1
    bool keep = false;
    bool steer_pair = false;
};

constexpr Button kButtons[] = {
    // Gameplay driving controls (auto-synced with MC3D's in-game Controller Configuration / Custom Remap!)
    {"steer_left",  "Steer Left",      SvgIcon::SteerLeft,      kSteerLeft,    game::GameAction::SteerAxis,      When::PlaySteerBtn,   0.50f,       true,  true},
    {"steer_right", "Steer Right",     SvgIcon::SteerRight,     kSteerRight,   game::GameAction::SteerAxis,      When::PlaySteerBtn,   0.50f,       true,  true},
    {"steer_stick", "Steer Stick",     SvgIcon::Count,          kSteerStick,   game::GameAction::SteerAxis,      When::PlayStick,      1.0f,        true,  false},
    {"gas",         "Gas",             SvgIcon::Gas,            kAnalogRT,     game::GameAction::Gas,            When::Play,           0.50f,       true,  false},
    {"brake",       "Brake",           SvgIcon::Brake,          kAnalogLT,     game::GameAction::Brake,          When::Play,           2.0f / 3.0f, true,  false},
    {"handbrake",   "Handbrake",       SvgIcon::Handbrake,      kAnalogA,      game::GameAction::Handbrake,      When::Play,           1.0f,        false, false},
    {"weight_xfer", "Weight Transfer", SvgIcon::WeightTransfer, kAnalogB,      game::GameAction::WeightTransfer, When::Play,           1.0f,        false, false},
    {"burnout",     "Burnout",         SvgIcon::Burnout,        kBurnout,      game::GameAction::Count,          When::Play,           1.0f,        false, false},
    {"nitro",       "Nitro",           SvgIcon::Nitro,          kAnalogX,      game::GameAction::Nitro,          When::PlayNos,        1.0f,        false, false},
    {"turbo",       "Turbo",           SvgIcon::Turbo,          kAnalogX,      game::GameAction::Nitro,          When::PlayTurbo,      1.0f,        false, false},
    {"special",     "Special",         SvgIcon::Special,        kBit + 6,      game::GameAction::Special,        When::PlaySpecial,    1.0f,        false, false},
    {"pause",       "Pause",           SvgIcon::Pause,          kBit + 4,      game::GameAction::Pause,          When::Play,           1.0f,        true,  false},
    {"camera",      "Camera",          SvgIcon::Camera,         kAnalogBlack,  game::GameAction::Camera,         When::Play,           1.0f,        false, false},
    {"map",         "Map",             SvgIcon::Map,            kBit + 5,      game::GameAction::Map,            When::Play,           1.0f,        false, false},
    {"flash",       "Flash",           SvgIcon::FlashLights,    kAnalogWhite,  game::GameAction::FlashLights,    When::Play,           1.0f,        false, false},
    {"look_back",   "Look Back",       SvgIcon::LookBack,       kAnalogY,      game::GameAction::LookBack,       When::Play,           1.0f,        false, false},

    // Menu & Garage minimal navigation controls (NFSU2 wide pill OK/Back + dynamic Garage D-Pad)
    {"dpad_up",     "D-Pad Up",        SvgIcon::DpadUp,         kBit + 0,      game::GameAction::Count,          When::MenusAndGarage, 1.0f,        true,  false},
    {"dpad_down",   "D-Pad Down",      SvgIcon::DpadDown,       kBit + 1,      game::GameAction::Count,          When::MenusAndGarage, 1.0f,        true,  false},
    {"dpad_left",   "D-Pad Left",      SvgIcon::DpadLeft,       kBit + 2,      game::GameAction::Count,          When::MenusAndGarage, 1.0f,        true,  false},
    {"dpad_right",  "D-Pad Right",     SvgIcon::DpadRight,      kBit + 3,      game::GameAction::Count,          When::MenusAndGarage, 1.0f,        true,  false},
    {"ok",          "OK",              SvgIcon::Ok,             kAnalogA,      game::GameAction::Count,          When::MenusAndGarage, 2.375f,      true,  false},
    {"back",        "Back",            SvgIcon::Back,           kAnalogB,      game::GameAction::Count,          When::MenusAndGarage, 2.375f,      true,  false},
};

constexpr size_t kButtonCount = std::size(kButtons);
constexpr size_t kIdxSteerLeft = 0;
constexpr size_t kIdxSteerRight = 1;
constexpr size_t kIdxSteerStick = 2;
constexpr size_t kIdxGas = 3;
constexpr size_t kIdxBrake = 4;
constexpr size_t kIdxHandbrake = 5;
constexpr size_t kIdxWeightTransfer = 6;
constexpr size_t kIdxBurnout = 7;
constexpr size_t kIdxNitro = 8;
constexpr size_t kIdxTurbo = 9;
constexpr size_t kIdxSpecial = 10;
constexpr size_t kIdxPause = 11;
constexpr size_t kIdxCamera = 12;
constexpr size_t kIdxMap = 13;
constexpr size_t kIdxFlash = 14;
constexpr size_t kIdxLookBack = 15;
constexpr size_t kIdxDpadUp = 16;
constexpr size_t kIdxDpadDown = 17;
constexpr size_t kIdxDpadLeft = 18;
constexpr size_t kIdxDpadRight = 19;
constexpr size_t kIdxOk = 20;
constexpr size_t kIdxBack = 21;

// Screen-percentage placement:
//   x, y in [0..1] screen shares
//   r = half-height as share of screen height (drawn H = 2*r*scale*ScreenH, W = H * w_over_h)
struct Place {
    float x = 0.5f, y = 0.5f, r = 0.07f;
    bool hidden = false;
};
using Layout = std::array<Place, kButtonCount>;

// Dynamic percentage-based aspect-aware layout so buttons match ANY screen ratio (16:9, 19.5:9, 20:9, 21:9, tablet)
Layout ComputeDefaultLayout(float screen_w, float screen_h) {
    const float aspect = std::clamp(screen_w / std::max(screen_h, 1.0f), 1.33f, 2.55f);
    const float edge_x = 0.032f;  // 3.2% horizontal safe margin
    const float edge_y = 0.055f;  // 5.5% vertical safe margin

    auto rx_of = [aspect](float r, float w_over_h) { return (r * w_over_h) / aspect; };

    Layout L{};

    // --- Gameplay: 1:2 Steer Left/Right OR Analog Stick (bottom-left), 1:2 Gas & 2:3 Brake (bottom-right) ---
    const float steer_r = 0.175f;  // 35% screen height, 1:2 W:H
    const float steer_rx = rx_of(steer_r, 0.50f);
    const float steer_y = 1.0f - edge_y - steer_r;
    L[kIdxSteerLeft]  = {edge_x + steer_rx, steer_y, steer_r, false};
    L[kIdxSteerRight] = {edge_x + steer_rx * 3.35f, steer_y, steer_r, false};

    const float stick_r = 0.145f;  // 29% screen height circular thumbstick
    const float stick_rx = rx_of(stick_r, 1.0f);
    L[kIdxSteerStick] = {edge_x + stick_rx + 0.014f, 1.0f - edge_y - stick_r, stick_r, false};

    const float gas_r = 0.185f;    // 37% screen height, 1:2 W:H
    const float gas_rx = rx_of(gas_r, 0.50f);
    const float gas_x = 1.0f - edge_x - gas_rx;
    const float gas_y = 1.0f - edge_y - gas_r;
    L[kIdxGas] = {gas_x, gas_y, gas_r, false};

    const float brk_r = 0.140f;    // 28% screen height, 2:3 W:H
    const float brk_rx = rx_of(brk_r, 2.0f / 3.0f);
    const float brk_x = gas_x - gas_rx - brk_rx - 0.022f;
    const float brk_y = 1.0f - edge_y - brk_r;
    L[kIdxBrake] = {brk_x, brk_y, brk_r, false};

    const float hb_r = 0.068f;
    const float hb_y = brk_y - brk_r - hb_r - 0.045f;
    L[kIdxHandbrake] = {brk_x, hb_y, hb_r, false};

    const float wt_r = 0.066f;
    L[kIdxWeightTransfer] = {brk_x, hb_y - hb_r - wt_r - 0.036f, wt_r, false};

    const float nitro_r = 0.075f;
    L[kIdxNitro] = {gas_x, gas_y - gas_r - nitro_r - 0.045f, nitro_r, false};

    const float turbo_r = 0.078f;
    const float turbo_rx = rx_of(turbo_r, 1.0f);
    const float aux_x = brk_x - brk_rx - turbo_rx - 0.024f;
    L[kIdxBurnout] = {aux_x, hb_y - 0.045f, 0.064f, false};
    L[kIdxTurbo]   = {aux_x, brk_y - brk_r * 0.85f, turbo_r, false};
    L[kIdxSpecial] = {aux_x, brk_y + brk_r * 0.25f, 0.074f, false};

    const float top_r = 0.048f;
    const float top_rx = rx_of(top_r, 1.0f);
    const float top_y = 0.082f;
    L[kIdxPause]    = {0.500f, top_y, top_r, false};
    L[kIdxCamera]   = {0.500f - top_rx * 2.55f, top_y, top_r, false};
    L[kIdxMap]      = {0.500f + top_rx * 2.55f, top_y, top_r, false};
    L[kIdxLookBack] = {0.500f - top_rx * 5.10f, top_y, top_r, true};
    L[kIdxFlash]    = {gas_x, 0.165f, 0.056f, false};

    // --- Menus & Garage: 1.25x larger D-Pad, OK, and Back dynamically matched to screen ---
    const float dp_r = 0.0725f;  // 1.25x of 0.058 (14.5% screen height per tile)
    const float dp_rx = rx_of(dp_r, 1.0f);
    const float dp_step_x = dp_rx * 2.12f;
    const float dp_step_y = dp_r * 2.12f;
    const float dp_cx = edge_x + dp_rx + dp_step_x;
    const float dp_cy = 1.0f - edge_y - dp_r - dp_step_y;
    L[kIdxDpadUp]    = {dp_cx, dp_cy - dp_step_y, dp_r, false};
    L[kIdxDpadDown]  = {dp_cx, dp_cy + dp_step_y, dp_r, false};
    L[kIdxDpadLeft]  = {dp_cx - dp_step_x, dp_cy, dp_r, false};
    L[kIdxDpadRight] = {dp_cx + dp_step_x, dp_cy, dp_r, false};

    // NFSU2-style wide horizontal pill OK & Back stacked vertically on bottom-right (114x48 dp -> 2.375:1)
    const float pill_r = 0.064f;   // 12.8% screen height -> 30.4% screen height wide
    const float pill_rx = rx_of(pill_r, 2.375f);
    const float pill_x = 1.0f - edge_x - pill_rx;
    const float ok_y = 1.0f - edge_y - pill_r;
    const float back_y = ok_y - pill_r * 2.0f - 0.032f;
    L[kIdxOk]   = {pill_x, ok_y, pill_r, false};
    L[kIdxBack] = {pill_x, back_y, pill_r, false};

    return L;
}

constexpr float kFadeMs = 200.0f;         // 200ms dynamic fade-out on controller / fade-in on touch
constexpr uint64_t kBackTapMs = 120;
constexpr uint64_t kPulseMs = 105;
constexpr uint64_t kToastMs = 800;
constexpr float kFmvHoldSkipMs = 850.0f;  // Hold anywhere 850ms during FMV to fill ring & skip
constexpr uint64_t kFmvSkipPulseMs = 180;

struct Finger {
    SDL_FingerID id = 0;
    Role role = Role::None;
    size_t button = 0;
    float x0 = 0, y0 = 0;
    float x = 0, y = 0;
    uint64_t down_ms = 0;
    bool swipe_fired = false;
    float notch_x = 0;
    float notch_y = 0;
};

std::mutex g_lock;
std::vector<Finger> g_fingers;
bool g_pressed[kButtonCount] = {};
std::atomic<uint64_t> g_back_until{0};

// Smooth 200ms dynamic auto-fade state (under g_lock)
float g_fade_target = 1.0f;
float g_fade_from = 1.0f;
uint64_t g_fade_start_ms = 0;

// Radio swipe & Garage vertical swipe short pulses
uint64_t g_pulse_until[4] = {};  // 0=DUp, 1=DDown, 2=DLeft, 3=DRight
uint64_t g_radio_toast_until = 0;
int g_radio_toast_dir = 0;

// FMV hold-anywhere skip animation state (under g_lock)
float g_fmv_progress = 0.0f;
uint64_t g_fmv_last_ms = 0;
uint64_t g_fmv_skip_pulse_until = 0;
bool g_fmv_skip_fired = false;

// Analog steering stick & Garage 1-finger orbit camera state (under g_lock)
bool g_analog_stick = false;
float g_stick_raw_x = 0.0f;
float g_stick_raw_y = 0.0f;
float g_stick_x = 0.0f;
float g_stick_y = 0.0f;
float g_orbit_x = 0.0f;
float g_orbit_y = 0.0f;

Layout g_layout{};
uint32_t g_skin = kOutline;
float g_opacity = 0.85f;
float g_scale = 1.0f;
bool g_loaded = false;
float g_layout_w = 1920.0f, g_layout_h = 1080.0f;

std::atomic<bool> g_editing{false};
bool g_edit_menu_tab = false;  // false = Gameplay controls, true = Menu controls
bool g_snap_grid = true;       // #7 QoL: Snap-to-grid + magnetic partner/symmetry alignment
float g_guide_x = -1.0f;       // Active magnetic vertical alignment guide (0..1, <0 hidden)
float g_guide_y = -1.0f;       // Active magnetic horizontal alignment guide (0..1, <0 hidden)
float g_guide_mirror_x = -1.0f;// Active mirror symmetry partner guide (0..1, <0 hidden)
float g_bar_cx = 0.50f;        // Draggable NFSU2-style editor card center X (0..1)
float g_bar_top = 0.032f;      // Draggable NFSU2-style editor card top Y (0..1)
bool g_bar_dragging = false;   // Highlights card border in NFSU2 lime while dragging
Layout g_before{};
float g_before_opacity = 0.85f;
int g_selected = -1;

// Atlas UV rects for SVG icons + anti-aliased proportional Roboto font glyphs (ASCII 32..126)
struct AtlasUv {
    float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
    bool valid = false;
};
struct GlyphUv {
    float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
    float xoff = 0, yoff = 0;  // normalized to raster pixel height
    float w = 0, h = 0;        // normalized to raster pixel height
    float adv = 0.28f;         // normalized advance width
    bool valid = false;
};
AtlasUv g_atlas_uv[kSkins][kSvgIconCount]{};
GlyphUv g_font_uv[95]{};  // ASCII 32..126
float g_font_cap_center = 0.36f;  // baseline offset from vertical center (fraction of line_h)
std::atomic<bool> g_skin_ready{false};

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

float CurrentFadeAlphaLocked(uint64_t now) {
    if (g_editing) return 1.0f;
    if (g_fade_start_ms == 0 || now <= g_fade_start_ms) return g_fade_target;
    float t = float(now - g_fade_start_ms) / kFadeMs;
    if (t >= 1.0f) return g_fade_target;
    float ease = t * (2.0f - t);
    return std::clamp(g_fade_from + (g_fade_target - g_fade_from) * ease, 0.0f, 1.0f);
}

void StartFadeLocked(float target, uint64_t now) {
    if (std::fabs(g_fade_target - target) < 0.01f) return;
    g_fade_from = CurrentFadeAlphaLocked(now);
    g_fade_target = target;
    g_fade_start_ms = now;
}

std::filesystem::path LayoutFile() { return app::ExeDir() / "layout.toml"; }

void LoadLayout() {
    g_loaded = true;
    g_layout_w = Width();
    g_layout_h = Height();
    g_layout = ComputeDefaultLayout(g_layout_w, g_layout_h);
    try {
        toml::table t = toml::parse_file(LayoutFile().string());
        g_opacity = std::clamp(float(t["opacity"].value_or(double(g_opacity))), 0.2f, 1.0f);
        g_scale = std::clamp(float(t["scale"].value_or(double(g_scale))), 0.5f, 1.5f);
        g_analog_stick = t["analog_stick"].value_or(false);
        std::string skin = t["skin"].value_or(std::string(kSkinDirs[g_skin]));
        for (uint32_t i = 0; i < kSkins; i++) {
            if (skin == kSkinDirs[i]) g_skin = i;
        }
        // layout_version >= 4 preserves user positions (new buttons default cleanly from ComputeDefaultLayout)
        if (t["layout_version"].value_or(0) >= 4) {
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
        {"layout_version", 5},
        {"opacity", double(g_opacity)},
        {"scale", double(g_scale)},
        {"analog_stick", g_analog_stick},
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
    return w == When::Play || w == When::PlaySteerBtn || w == When::PlayStick ||
           w == When::PlayNos || w == When::PlayTurbo || w == When::PlaySpecial;
}

bool GarageVerticalMode() {
    return !g_editing && CurrentUiContext() == game::UiContext::Garage &&
           game::Telemetry().garage_vertical.load(std::memory_order_relaxed);
}

bool IsGarageHorizontalDpad(size_t i) {
    return !g_editing && CurrentUiContext() == game::UiContext::Garage && !GarageVerticalMode() &&
           (i == kIdxDpadLeft || i == kIdxDpadRight);
}

bool IsGarageVerticalDpad(size_t i) {
    return !g_editing && CurrentUiContext() == game::UiContext::Garage && GarageVerticalMode() &&
           (i == kIdxDpadUp || i == kIdxDpadDown);
}

bool ButtonShown(size_t i) {
    const Button& b = kButtons[i];
    if (g_editing) {
        if (g_edit_menu_tab) return !IsGameplayButton(b.when);
        if (b.when == When::PlaySteerBtn) return !g_analog_stick;
        if (b.when == When::PlayStick) return g_analog_stick;
        return IsGameplayButton(b.when);
    }
    if (g_layout[i].hidden) return false;

    const auto ctx = CurrentUiContext();
    if (ctx == game::UiContext::Loading || ctx == game::UiContext::FmvUnskippable ||
        ctx == game::UiContext::FmvSkippable) {
        return false;
    }
    if (ctx == game::UiContext::Garage) {
        // Garage dynamically swaps Left/Right (1:2) <-> Up/Down (2:1) based on active menu orientation
        const bool vert = game::Telemetry().garage_vertical.load(std::memory_order_relaxed);
        if (i == kIdxDpadUp || i == kIdxDpadDown) return vert;
        if (i == kIdxDpadLeft || i == kIdxDpadRight) return !vert;
    }
    switch (b.when) {
        case When::MainMenuOnly:
            return ctx == game::UiContext::MainMenu;
        case When::MenusAndGarage:
            return ctx == game::UiContext::MainMenu || ctx == game::UiContext::Garage;
        case When::Play:
            return ctx == game::UiContext::Gameplay;
        case When::PlaySteerBtn:
            return ctx == game::UiContext::Gameplay && !g_analog_stick;
        case When::PlayStick:
            return ctx == game::UiContext::Gameplay && g_analog_stick;
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
    const float w = Width();
    const float h = Height();
    float cx = g_layout[i].x * w;
    float cy = g_layout[i].y * h;
    float half_h = g_layout[i].r * g_scale * h;
    float aspect_wh = kButtons[i].w_over_h;

    if (IsGarageHorizontalDpad(i)) {
        // Garage Horizontal mode (Depth 0): 1:2 W:H Left & Right side-by-side at D-Pad center
        const float mid_x = 0.5f * (g_layout[kIdxDpadLeft].x + g_layout[kIdxDpadRight].x) * w;
        const float mid_y = 0.5f * (g_layout[kIdxDpadUp].y + g_layout[kIdxDpadDown].y) * h;
        const float base_r = g_layout[kIdxDpadLeft].r * g_scale * h;
        aspect_wh = 0.50f;
        half_h = base_r * 2.0f;  // 1:2 tall (width = 2*base_r, height = 4*base_r)
        const float half_w = half_h * aspect_wh;  // = base_r
        cx = mid_x + (i == kIdxDpadLeft ? -half_w * 1.16f : half_w * 1.16f);
        cy = mid_y;
    } else if (IsGarageVerticalDpad(i)) {
        // Garage Vertical mode (Depth >= 1): 2:1 W:H Up & Down stacked top-down (exact transpose of 1:2!)
        const float mid_x = 0.5f * (g_layout[kIdxDpadLeft].x + g_layout[kIdxDpadRight].x) * w;
        const float mid_y = 0.5f * (g_layout[kIdxDpadUp].y + g_layout[kIdxDpadDown].y) * h;
        const float base_r = g_layout[kIdxDpadLeft].r * g_scale * h;
        aspect_wh = 2.0f;
        half_h = base_r;         // 2:1 wide (width = 4*base_r, height = 2*base_r -> identical area!)
        cx = mid_x;
        cy = mid_y + (i == kIdxDpadUp ? -half_h * 1.18f : half_h * 1.18f);
    }

    const float half_w = half_h * aspect_wh;
    x0 = cx - half_w;
    y0 = cy - half_h;
    x1 = cx + half_w;
    y1 = cy + half_h;
}

size_t HitButton(float x, float y) {
    for (size_t i = 0; i < kButtonCount; i++) {
        if (!ButtonShown(i)) continue;
        float x0, y0, x1, y1;
        ButtonRect(i, x0, y0, x1, y1);
        const float cx = (x0 + x1) * 0.5f;
        const float cy = (y0 + y1) * 0.5f;
        const float hw = (x1 - x0) * 0.5f;
        const float hh = (y1 - y0) * 0.5f;
        const float aspect = hw / std::max(hh, 1.0f);
        if (aspect < 0.95f || aspect > 1.05f) {
            if (std::fabs(x - cx) <= hw * 1.12f && std::fabs(y - cy) <= hh * 1.16f) return i;
        } else {
            const float r = hh * 1.18f;
            if ((x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r) return i;
        }
    }
    return kButtonCount;
}

// ============================================================================
// NFSU2-Style Draggable Floating Layout Editor Card (Zero ImGui, Google Roboto)
// ============================================================================
enum EditorBtn : int {
    kEdReset = 0,
    kEdMinus,
    kEdPlus,
    kEdSave,
    kEdCancel,
    kEdTab,
    kEdSnap,
    kEdHide,
    kEdCount,
};
constexpr float kMinRadius = 0.035f, kMaxRadius = 0.28f;
constexpr float kSizeStep = 1.1f;

bool Hideable(int i) { return i >= 0 && size_t(i) < kButtonCount && !kButtons[size_t(i)].keep; }

struct EditorCard {
    float card[4];
    float grip[4];
    float btns[kEdCount][4];
    float status_cy;
};

void EditorCardSize(float w, float h, float& card_w, float& card_h) {
    // Generous mobile-first NFSU2 card dimensions (~10.4% screen height per button row)
    card_h = h * 0.255f;
    card_w = std::min(w * 0.92f, h * 1.48f);
}

void ClampEditorCard(float w, float h, float card_w, float card_h) {
    const float half_w = card_w * 0.5f;
    const float min_cx = (half_w + w * 0.012f) / w;
    const float max_cx = std::max(min_cx, 1.0f - min_cx);
    const float min_top = 0.014f;
    const float max_top = std::max(min_top, 1.0f - (card_h / h) - 0.014f);
    g_bar_cx = std::clamp(g_bar_cx, min_cx, max_cx);
    g_bar_top = std::clamp(g_bar_top, min_top, max_top);
}

EditorCard ComputeEditorCard() {
    const float w = Width(), h = Height();
    float card_w = 0.0f, card_h = 0.0f;
    EditorCardSize(w, h, card_w, card_h);
    ClampEditorCard(w, h, card_w, card_h);

    EditorCard c{};
    const float x0 = g_bar_cx * w - card_w * 0.5f;
    const float y0 = g_bar_top * h;
    c.card[0] = x0;
    c.card[1] = y0;
    c.card[2] = x0 + card_w;
    c.card[3] = y0 + card_h;

    const float pad = card_h * 0.080f;
    const float gap = card_h * 0.070f;
    const float grip_w = card_h * 0.30f;
    c.grip[0] = x0;
    c.grip[1] = y0;
    c.grip[2] = x0 + grip_w;
    c.grip[3] = y0 + card_h;

    const float inner_x0 = x0 + grip_w + gap * 0.4f;
    const float inner_w = (x0 + card_w - pad) - inner_x0;
    const float row_h = (card_h - pad * 2.0f - gap) * 0.5f;
    const float r1_y0 = y0 + pad;
    const float r1_y1 = r1_y0 + row_h;
    const float r2_y0 = r1_y1 + gap;
    const float r2_y1 = r2_y0 + row_h;

    // Row 1: RESET | - | + | SAVE | CANCEL (matches NFSU2 RacingControlsView layout)
    const float r1_weights[5] = {0.22f, 0.12f, 0.12f, 0.28f, 0.26f};
    const float r1_avail = inner_w - gap * 4.0f;
    float cx = inner_x0;
    for (int i = 0; i < 5; ++i) {
        const float bw = r1_avail * r1_weights[i];
        c.btns[i][0] = cx;
        c.btns[i][1] = r1_y0;
        c.btns[i][2] = cx + bw;
        c.btns[i][3] = r1_y1;
        cx += bw + gap;
    }

    // Row 2: PLAY/MENU | SNAP ON/OFF | HIDE/SHOW
    const float r2_weights[3] = {0.34f, 0.33f, 0.33f};
    const float r2_avail = inner_w - gap * 2.0f;
    cx = inner_x0;
    for (int i = 0; i < 3; ++i) {
        const float bw = r2_avail * r2_weights[i];
        const int bi = kEdTab + i;
        c.btns[bi][0] = cx;
        c.btns[bi][1] = r2_y0;
        c.btns[bi][2] = cx + bw;
        c.btns[bi][3] = r2_y1;
        cx += bw + gap;
    }

    // Place status badge below card (or above if card was dragged near bottom edge)
    c.status_cy = (c.card[3] + h * 0.085f < h) ? (c.card[3] + h * 0.048f) : (c.card[1] - h * 0.048f);
    return c;
}

void OpenEditor() {
    g_before = g_layout;
    g_before_opacity = g_opacity;
    g_selected = -1;
    g_edit_menu_tab = false;
    g_bar_cx = 0.50f;
    g_bar_top = 0.030f;
    g_bar_dragging = false;
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
    g_bar_dragging = false;
    g_guide_x = g_guide_y = g_guide_mirror_x = -1.0f;
    g_editing = false;
}

void Resize(size_t i, float r) {
    const float clamped = std::clamp(r, kMinRadius, kMaxRadius);
    g_layout[i].r = clamped;
    // Symmetry pair sizing when SNAP is active: keep Steer L/R and D-Pad tiles uniform
    if (g_snap_grid) {
        if (i == kIdxSteerLeft || i == kIdxSteerRight) {
            g_layout[kIdxSteerLeft].r = clamped;
            g_layout[kIdxSteerRight].r = clamped;
        } else if (i == kIdxDpadUp || i == kIdxDpadDown || i == kIdxDpadLeft || i == kIdxDpadRight) {
            g_layout[kIdxDpadUp].r = clamped;
            g_layout[kIdxDpadDown].r = clamped;
            g_layout[kIdxDpadLeft].r = clamped;
            g_layout[kIdxDpadRight].r = clamped;
        }
    }
}

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
    const EditorCard c = ComputeEditorCard();
    auto in_rect = [x, y](const float r[4]) { return x >= r[0] && x <= r[2] && y >= r[1] && y <= r[3]; };

    // Inside floating NFSU2 editor card?
    if (in_rect(c.card)) {
        // Left grip handle -> drag toolbar card immediately
        if (in_rect(c.grip)) {
            f.role = Role::ToolbarDrag;
            f.x0 = x - g_bar_cx * Width();
            f.y0 = y - g_bar_top * Height();
            g_bar_dragging = true;
            return;
        }
        // Check action buttons inside card
        for (int i = 0; i < kEdCount; ++i) {
            if (!in_rect(c.btns[i])) continue;
            switch (i) {
                case kEdReset:
                    g_layout = ComputeDefaultLayout(Width(), Height());
                    g_selected = -1;
                    g_guide_x = g_guide_y = g_guide_mirror_x = -1.0f;
                    return;
                case kEdMinus:
                    if (g_selected >= 0 && size_t(g_selected) < kButtonCount) {
                        Resize(size_t(g_selected), g_layout[size_t(g_selected)].r / kSizeStep);
                    }
                    return;
                case kEdPlus:
                    if (g_selected >= 0 && size_t(g_selected) < kButtonCount) {
                        Resize(size_t(g_selected), g_layout[size_t(g_selected)].r * kSizeStep);
                    }
                    return;
                case kEdSave:
                    CloseEditor(true);
                    return;
                case kEdCancel:
                    CloseEditor(false);
                    return;
                case kEdTab:
                    g_edit_menu_tab = !g_edit_menu_tab;
                    g_selected = -1;
                    g_guide_x = g_guide_y = g_guide_mirror_x = -1.0f;
                    return;
                case kEdSnap:
                    g_snap_grid = !g_snap_grid;
                    g_guide_x = g_guide_y = g_guide_mirror_x = -1.0f;
                    return;
                case kEdHide:
                    if (Hideable(g_selected)) g_layout[size_t(g_selected)].hidden ^= true;
                    return;
                default:
                    return;
            }
        }
        // Touched card background outside buttons -> drag toolbar card anywhere on screen!
        f.role = Role::ToolbarDrag;
        f.x0 = x - g_bar_cx * Width();
        f.y0 = y - g_bar_top * Height();
        g_bar_dragging = true;
        return;
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

void UpdateAnalogStickLocked(float x, float y) {
    float x0, y0, x1, y1;
    ButtonRect(kIdxSteerStick, x0, y0, x1, y1);
    const float cx = (x0 + x1) * 0.5f;
    const float cy = (y0 + y1) * 0.5f;
    const float radius = std::max((y1 - y0) * 0.5f, 1.0f);
    float dx = (x - cx) / radius;
    float dy = (y - cy) / radius;
    const float mag = std::hypot(dx, dy);
    if (mag > 1.0f) {
        dx /= mag;
        dy /= mag;
    }
    auto shape_axis = [](float v) -> float {
        constexpr float kDeadzone = 0.04f;
        const float av = std::fabs(v);
        if (av <= kDeadzone) return 0.0f;
        const float norm = std::clamp((av - kDeadzone) / (1.0f - kDeadzone), 0.0f, 1.0f);
        // Progressive NFSU2 S-curve: crisp center precision with responsive full lock
        const float curved = norm * norm * (0.45f + 0.55f * norm) * 0.65f + norm * 0.35f;
        return std::copysign(std::clamp(curved, 0.0f, 1.0f), v);
    };
    g_stick_raw_x = dx;
    g_stick_raw_y = dy;
    g_stick_x = shape_axis(dx);
    g_stick_y = shape_axis(dy);
}

void FingerDown(SDL_FingerID id, float x, float y) {
    const uint64_t now = NowMs();
    StartFadeLocked(1.0f, now);  // Smooth 200ms fade-in whenever screen is touched!

    Finger f;
    f.id = id;
    f.x0 = f.x = x;
    f.y0 = f.y = y;
    f.down_ms = now;
    if (g_editing) {
        EditDown(f, x, y);
        g_fingers.push_back(f);
        return;
    }
    input::g_touch = true;

    const auto ctx = CurrentUiContext();
    if (ctx == game::UiContext::FmvSkippable) {
        // Touch & hold ANYWHERE on screen to animate start.svg circular ring and skip FMV!
        f.role = Role::FmvHold;
        g_fmv_skip_fired = false;
        g_fingers.push_back(f);
        return;
    }
    if (ctx == game::UiContext::FmvUnskippable || ctx == game::UiContext::Loading) {
        g_fingers.push_back(f);
        return;
    }

    const size_t b = HitButton(x, y);
    if (b == kIdxSteerStick && g_analog_stick && ctx == game::UiContext::Gameplay) {
        f.role = Role::AnalogStick;
        f.button = kIdxSteerStick;
        UpdateAnalogStickLocked(x, y);
    } else if (b < kButtonCount) {
        f.role = Role::Button;
        f.button = b;
    } else {
        const float w = Width();
        const float h = Height();
        if (ctx == game::UiContext::Garage) {
            // #2 UX: 1-finger drag anywhere in open car viewport orbits garage camera (Right Stick)
            f.role = Role::GarageOrbit;
        } else if (ctx == game::UiContext::Gameplay &&
                   x >= w * 0.30f && x <= w * 0.70f && y >= h * 0.72f) {
            f.role = Role::RadioSwipe;
        } else {
            // NFSU2-style free slide-across: start on open screen and drag into any button without lifting!
            f.role = Role::Button;
            f.button = kButtonCount;
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
        if (f.role == Role::ToolbarDrag) {
            float card_w = 0.0f, card_h = 0.0f;
            EditorCardSize(w, h, card_w, card_h);
            g_bar_cx = (x - f.x0) / w;
            g_bar_top = (y - f.y0) / h;
            ClampEditorCard(w, h, card_w, card_h);
            continue;
        }
        if (f.role == Role::Drag) {
            Place& p = g_layout[f.button];
            float nx = std::clamp((x - f.x0) / w, 0.02f, 0.98f);
            float ny = std::clamp((y - f.y0) / h, 0.04f, 0.96f);
            g_guide_x = g_guide_y = g_guide_mirror_x = -1.0f;
            if (g_snap_grid) {
                bool snapped_x = false, snapped_y = false;
                // 1. Magnetic center-screen alignment
                if (std::fabs(nx - 0.50f) < 0.012f) { nx = 0.50f; g_guide_x = 0.50f; snapped_x = true; }
                if (std::fabs(ny - 0.50f) < 0.014f) { ny = 0.50f; g_guide_y = 0.50f; snapped_y = true; }
                // 2. Magnetic partner & horizontal mirror symmetry alignment with other visible buttons
                float best_dx = 0.012f, best_dy = 0.014f, best_mx = 0.012f;
                for (size_t j = 0; j < kButtonCount; ++j) {
                    if (j == f.button || !ButtonShown(j)) continue;
                    const Place& o = g_layout[j];
                    if (!snapped_y) {
                        const float dy = std::fabs(ny - o.y);
                        if (dy < best_dy) {
                            best_dy = dy;
                            ny = o.y;
                            g_guide_y = o.y;
                        }
                    }
                    if (!snapped_x) {
                        const float dx = std::fabs(nx - o.x);
                        if (dx < best_dx) {
                            best_dx = dx;
                            nx = o.x;
                            g_guide_x = o.x;
                            g_guide_mirror_x = -1.0f;
                        }
                        const float mx = 1.0f - o.x;
                        const float dmx = std::fabs(nx - mx);
                        if (dmx < best_mx && dmx < best_dx) {
                            best_mx = dmx;
                            nx = mx;
                            g_guide_x = mx;
                            g_guide_mirror_x = o.x;
                        }
                    }
                }
                // 3. Soft 2.5% grid snap on free axes
                if (g_guide_x < 0.0f) nx = std::clamp(std::round(nx / 0.025f) * 0.025f, 0.02f, 0.98f);
                if (g_guide_y < 0.0f) ny = std::clamp(std::round(ny / 0.025f) * 0.025f, 0.04f, 0.96f);
            }
            p.x = nx;
            p.y = ny;
        }
        if (f.role == Role::Drag || f.role == Role::Pinch) {
            for (const Finger& o : g_fingers) {
                const Finger& pinch = f.role == Role::Pinch ? f : o;
                const Finger& drag = f.role == Role::Pinch ? o : f;
                if (&o == &f || pinch.role != Role::Pinch || drag.role != Role::Drag || pinch.button != drag.button) continue;
                Resize(f.button, pinch.y0 * std::hypot(pinch.x - drag.x, pinch.y - drag.y) / pinch.x0);
            }
        }
        if (f.role == Role::AnalogStick) {
            UpdateAnalogStickLocked(x, y);
            continue;
        }
        if (f.role == Role::Button) {
            // NFSU2-style free slide-across touching: glide seamlessly into or between any buttons!
            const size_t hit = HitButton(x, y);
            if (hit == kIdxPause && f.button != kIdxPause) {
                // Guard: never accidentally slide-trigger Pause while gliding across screen
                f.button = kButtonCount;
            } else if (hit == kIdxSteerStick) {
                const bool stick_busy = std::any_of(g_fingers.begin(), g_fingers.end(),
                    [&](const Finger& o) { return &o != &f && o.role == Role::AnalogStick; });
                if (!stick_busy && f.button == kButtonCount) {
                    f.role = Role::AnalogStick;
                    f.button = kIdxSteerStick;
                    UpdateAnalogStickLocked(x, y);
                }
            } else if (hit != f.button) {
                f.button = hit;
                f.down_ms = now;
            }
        }
        if (f.role == Role::RadioSwipe && !f.swipe_fired) {
            const size_t hit = HitButton(x, y);
            if (hit < kButtonCount && hit != kIdxPause && hit != kIdxSteerStick) {
                // Finger glided into a driving button before triggering swipe -> seamlessly engage it!
                f.role = Role::Button;
                f.button = hit;
                f.down_ms = now;
                continue;
            }
            const float dx = x - f.x0;
            const float dy = y - f.y0;
            if (std::fabs(dx) >= w * 0.06f && std::fabs(dx) > std::fabs(dy) * 1.25f) {
                f.swipe_fired = true;
                if (dx > 0.0f) {
                    g_pulse_until[3] = now + kPulseMs;
                    g_radio_toast_dir = 1;
                } else {
                    g_pulse_until[2] = now + kPulseMs;
                    g_radio_toast_dir = -1;
                }
                g_radio_toast_until = now + kToastMs;
            }
        }
        if (f.role == Role::GarageOrbit) {
            // #2 UX: Smooth 1-finger garage orbit camera drag -> Right Stick (rx / ry)
            const float dx = x - f.x0;
            const float dy = y - f.y0;
            if (std::hypot(dx, dy) > h * 0.012f) {
                const float max_x = w * 0.14f;
                const float max_y = h * 0.18f;
                g_orbit_x = std::clamp(dx / max_x, -1.0f, 1.0f);
                g_orbit_y = std::clamp(-dy / max_y, -1.0f, 1.0f);
                // Trailing anchor so reversing orbit direction responds instantly
                if (std::fabs(dx) > max_x) f.x0 = x - std::copysign(max_x, dx);
                if (std::fabs(dy) > max_y) f.y0 = y - std::copysign(max_y, dy);
            }
        }
    }
    Recount();
}

void FingerUp(SDL_FingerID id) {
    std::erase_if(g_fingers, [id](const Finger& f) { return f.id == id; });
    if (std::none_of(g_fingers.begin(), g_fingers.end(),
                     [](const Finger& f) { return f.role == Role::Drag; })) {
        g_guide_x = g_guide_y = g_guide_mirror_x = -1.0f;
    }
    if (std::none_of(g_fingers.begin(), g_fingers.end(),
                     [](const Finger& f) { return f.role == Role::ToolbarDrag; })) {
        g_bar_dragging = false;
    }
    if (std::none_of(g_fingers.begin(), g_fingers.end(),
                     [](const Finger& f) { return f.role == Role::AnalogStick; })) {
        g_stick_raw_x = g_stick_raw_y = g_stick_x = g_stick_y = 0.0f;
    }
    if (std::none_of(g_fingers.begin(), g_fingers.end(),
                     [](const Finger& f) { return f.role == Role::GarageOrbit; })) {
        g_orbit_x = g_orbit_y = 0.0f;
    }
    Recount();
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
    {
        std::lock_guard lock(g_lock);
        StartFadeLocked(0.0f, NowMs());  // External keyboard smoothly fades out touch controls
    }
    if (!k.repeat && input::HudKey(vk)) game::g_hide_hud = !game::g_hide_hud;
    if (vk == VK_BACK) input::OnChar('\b');
    if (vk == VK_RETURN) input::OnChar('\r');
}

// ============================================================================
// Anti-Aliased Proportional Google Roboto Font Atlas (stb_truetype — Zero ImGui!)
// ============================================================================
bool LoadRobotoTtfBytes(std::vector<uint8_t>& out) {
    // 1. Primary: bundled Google Roboto-Bold.ttf inside APK assets (identical across all OEMs)
    size_t sz = 0;
    void* data = SDL_LoadFile("fonts/Roboto-Bold.ttf", &sz);
    if (data && sz > 0) {
        out.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + sz);
        SDL_free(data);
        LOGI("[OK] touch: loaded bundled Roboto-Bold.ttf (%zu bytes)", out.size());
        return true;
    }
    if (data) SDL_free(data);

    // 2. Fallback: Android system Roboto font files
    for (const char* path : {"/system/fonts/Roboto-Bold.ttf", "/system/fonts/Roboto-Medium.ttf",
                             "/system/fonts/Roboto-Regular.ttf"}) {
        std::ifstream f(path, std::ios::binary);
        if (!f) continue;
        out.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (!out.empty()) {
            LOGI("[OK] touch: loaded system Roboto font %s (%zu bytes)", path, out.size());
            return true;
        }
    }
    LOGW("touch: failed to load Roboto TTF font");
    return false;
}

void BakeRobotoIntoAtlas(std::vector<uint8_t>& rgba, uint32_t atlas_w, uint32_t atlas_h, uint32_t base_y) {
    std::vector<uint8_t> ttf;
    if (!LoadRobotoTtfBytes(ttf)) return;

    stbtt_fontinfo font{};
    if (!stbtt_InitFont(&font, ttf.data(), stbtt_GetFontOffsetForIndex(ttf.data(), 0))) {
        LOGE("touch: stbtt_InitFont failed on Roboto TTF");
        return;
    }

    constexpr float kFontPx = 64.0f;
    constexpr uint32_t kPad = 4u;
    const float scale = stbtt_ScaleForPixelHeight(&font, kFontPx);

    // Measure capital 'H' vertical bounds for crisp optical centering inside pills & circles
    int hx0 = 0, hy0 = 0, hx1 = 0, hy1 = 0;
    stbtt_GetCodepointBitmapBox(&font, 'H', scale, scale, &hx0, &hy0, &hx1, &hy1);
    if (hy1 > hy0) {
        g_font_cap_center = -0.5f * float(hy0 + hy1) / kFontPx;
    } else {
        g_font_cap_center = 0.36f;
    }

    uint32_t cur_x = kPad;
    uint32_t cur_y = base_y + kPad;
    uint32_t row_h = 0u;
    int baked = 0;

    for (uint32_t g = 0; g < 95u; ++g) {
        g_font_uv[g] = GlyphUv{};
        const int ch = int(32u + g);
        int adv_w = 0, lsb = 0;
        stbtt_GetCodepointHMetrics(&font, ch, &adv_w, &lsb);
        g_font_uv[g].adv = float(adv_w) * scale / kFontPx;

        if (ch == ' ') continue;

        int bw = 0, bh = 0, xoff = 0, yoff = 0;
        unsigned char* bmp = stbtt_GetCodepointBitmap(&font, 0.0f, scale, ch, &bw, &bh, &xoff, &yoff);
        if (!bmp || bw <= 0 || bh <= 0) {
            if (bmp) stbtt_FreeBitmap(bmp, nullptr);
            continue;
        }

        if (cur_x + uint32_t(bw) + kPad > atlas_w) {
            cur_x = kPad;
            cur_y += row_h + kPad;
            row_h = 0u;
        }
        if (cur_y + uint32_t(bh) + kPad > atlas_h) {
            stbtt_FreeBitmap(bmp, nullptr);
            LOGE("touch: Roboto atlas overflow at ASCII %d", ch);
            break;
        }

        for (int y = 0; y < bh; ++y) {
            uint8_t* dst = &rgba[(size_t(cur_y + uint32_t(y)) * atlas_w + cur_x) * 4u];
            const unsigned char* src_row = bmp + size_t(y) * size_t(bw);
            for (int x = 0; x < bw; ++x) {
                dst[x * 4 + 0] = 255u;
                dst[x * 4 + 1] = 255u;
                dst[x * 4 + 2] = 255u;
                dst[x * 4 + 3] = src_row[x];
            }
        }
        stbtt_FreeBitmap(bmp, nullptr);

        g_font_uv[g] = GlyphUv{
            float(cur_x) / float(atlas_w),
            float(cur_y) / float(atlas_h),
            float(cur_x + uint32_t(bw)) / float(atlas_w),
            float(cur_y + uint32_t(bh)) / float(atlas_h),
            float(xoff) / kFontPx,
            float(yoff) / kFontPx,
            float(bw) / kFontPx,
            float(bh) / kFontPx,
            float(adv_w) * scale / kFontPx,
            true,
        };
        cur_x += uint32_t(bw) + kPad;
        row_h = std::max(row_h, uint32_t(bh));
        ++baked;
    }
    LOGI("[OK] touch: baked %d anti-aliased Roboto glyphs into atlas (y=%u..%u)", baked, base_y, cur_y + row_h);
}

void EmitPanel(std::vector<Quad>& out, float x0, float y0, float x1, float y1,
               float r, float g, float b, float a, float radius_px,
               float border_px = 0.0f, float br = 1.0f, float bg = 1.0f, float bb = 1.0f, float ba = 0.0f) {
    out.push_back(Quad{x0, y0, x1, y1, 0, 0, 1, 1, r, g, b, a, radius_px, border_px, br, bg, bb, ba, 1u});
}

void EmitRing(std::vector<Quad>& out, float cx, float cy, float radius_px, float thickness_px, float progress,
              float r, float g, float b, float a, float br, float bg, float bb, float ba) {
    out.push_back(Quad{cx - radius_px, cy - radius_px, cx + radius_px, cy + radius_px,
                       0, 0, 1, 1, r, g, b, a, progress, thickness_px, br, bg, bb, ba, 2u});
}

void EmitTextCentered(std::vector<Quad>& out, float cx, float cy, float line_h,
                      float r, float g, float b, float a, const char* text) {
    if (!text || !text[0] || !g_skin_ready.load(std::memory_order_relaxed)) return;
    const size_t len = std::strlen(text);
    const float tracking = line_h * 0.015f;
    float total_w = 0.0f;
    for (size_t i = 0; i < len; ++i) {
        unsigned char ch = static_cast<unsigned char>(text[i]);
        if (ch < 32u || ch > 126u) ch = ' ';
        const GlyphUv& uv = g_font_uv[ch - 32u];
        total_w += uv.adv * line_h + ((i + 1u < len) ? tracking : 0.0f);
    }
    float x = cx - total_w * 0.5f;
    const float baseline_y = cy + g_font_cap_center * line_h;
    for (size_t i = 0; i < len; ++i) {
        unsigned char ch = static_cast<unsigned char>(text[i]);
        if (ch < 32u || ch > 126u) ch = ' ';
        const GlyphUv& uv = g_font_uv[ch - 32u];
        if (ch != ' ' && uv.valid) {
            const float gx0 = x + uv.xoff * line_h;
            const float gy0 = baseline_y + uv.yoff * line_h;
            const float gx1 = gx0 + uv.w * line_h;
            const float gy1 = gy0 + uv.h * line_h;
            out.push_back(Quad{gx0, gy0, gx1, gy1, uv.u0, uv.v0, uv.u1, uv.v1,
                               r, g, b, a, 0, 0, 0, 0, 0, 0, 0u});
        }
        x += uv.adv * line_h + tracking;
    }
}

// Dynamic editor UI palette synced with active Button Style (Outline / Filled White / Filled Dark)
struct EditorTheme {
    float pill_r, pill_g, pill_b, pill_a;
    float pill_br, pill_bg, pill_bb, pill_ba;
    float pill_bw;
    float pill_tr, pill_tg, pill_tb;
    float act_r, act_g, act_b, act_a;
    float act_br, act_bg, act_bb, act_ba;
    float act_tr, act_tg, act_tb;
    float dock_r, dock_g, dock_b, dock_a;
    float dock_br, dock_bg, dock_bb, dock_ba;
    float dock_tr, dock_tg, dock_tb;
};

EditorTheme EditorThemeForSkin(uint32_t skin, float th) {
    if (skin == kFilledWhite) {
        return EditorTheme{
            0.96f, 0.97f, 0.99f, 0.92f,  1.00f, 1.00f, 1.00f, 0.98f,  th * 0.6f,  0.07f, 0.08f, 0.11f,
            0.09f, 0.10f, 0.13f, 0.96f,  1.00f, 1.00f, 1.00f, 0.95f,              1.00f, 1.00f, 1.00f,
            0.96f, 0.97f, 1.00f, 0.24f,  1.00f, 1.00f, 1.00f, 0.55f,              1.00f, 1.00f, 1.00f,
        };
    }
    if (skin == kFilledDark) {
        return EditorTheme{
            0.08f, 0.09f, 0.12f, 0.92f,  1.00f, 1.00f, 1.00f, 0.20f,  th * 0.7f,  0.97f, 0.98f, 1.00f,
            0.97f, 0.98f, 1.00f, 0.96f,  1.00f, 1.00f, 1.00f, 1.00f,              0.07f, 0.08f, 0.11f,
            0.05f, 0.06f, 0.08f, 0.68f,  1.00f, 1.00f, 1.00f, 0.16f,              0.96f, 0.97f, 1.00f,
        };
    }
    // kOutline: sleek transparent capsules with crisp pure-white vector borders matching outline/*.svg
    return EditorTheme{
        0.04f, 0.05f, 0.07f, 0.24f,  1.00f, 1.00f, 1.00f, 0.94f,  th * 1.15f, 1.00f, 1.00f, 1.00f,
        1.00f, 1.00f, 1.00f, 0.96f,  1.00f, 1.00f, 1.00f, 1.00f,              0.06f, 0.07f, 0.10f,
        0.04f, 0.05f, 0.07f, 0.40f,  1.00f, 1.00f, 1.00f, 0.45f,              1.00f, 1.00f, 1.00f,
    };
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
    } else if (icon == SvgIcon::DpadLeftGarage || icon == SvgIcon::DpadRightGarage) {
        ReplaceAll(svg, "viewBox=\"0 0 512 512\" width=\"512\" height=\"512\"",
                        "viewBox=\"0 0 512 1024\" width=\"512\" height=\"1024\"");
        ReplaceAll(svg, "width=\"512\" height=\"512\"", "width=\"512\" height=\"1024\"");
        ReplaceAll(svg, "x=\"12\" y=\"12\" width=\"104\" height=\"104\"",
                        "x=\"12\" y=\"12\" width=\"104\" height=\"232\"");
        ReplaceAll(svg, "<path ", "<path transform=\"translate(0 64)\" ");
    } else if (icon == SvgIcon::DpadUpGarage || icon == SvgIcon::DpadDownGarage) {
        ReplaceAll(svg, "viewBox=\"0 0 512 512\" width=\"512\" height=\"512\"",
                        "viewBox=\"0 0 1024 512\" width=\"1024\" height=\"512\"");
        ReplaceAll(svg, "width=\"512\" height=\"512\"", "width=\"1024\" height=\"512\"");
        ReplaceAll(svg, "x=\"12\" y=\"12\" width=\"104\" height=\"104\"",
                        "x=\"12\" y=\"12\" width=\"232\" height=\"104\"");
        ReplaceAll(svg, "<path ", "<path transform=\"translate(64 0)\" ");
    }
}

bool ReadSvgFile(const char* skin_dir, const char* icon_name, std::string& out) {
    const auto user_path = app::ExeDir() / "skins" / skin_dir / (std::string(icon_name) + ".svg");
    {
        std::ifstream f(user_path, std::ios::binary);
        if (f) {
            out.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            if (!out.empty()) return true;
        }
    }
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
    if (IsGarageHorizontalDpad(i)) {
        return (i == kIdxDpadLeft) ? SvgIcon::DpadLeftGarage : SvgIcon::DpadRightGarage;
    }
    if (IsGarageVerticalDpad(i)) {
        return (i == kIdxDpadUp) ? SvgIcon::DpadUpGarage : SvgIcon::DpadDownGarage;
    }
    if (b.when == When::PlaySpecial && !g_editing) {
        const int8_t st = game::Telemetry().special_type.load(std::memory_order_relaxed);
        if (st == 0) return SvgIcon::SpecialZone;
        if (st == 1) return SvgIcon::SpecialAgro;
        if (st == 2) return SvgIcon::SpecialRoar;
    }
    return b.icon;
}

void UpdateFmvHoldLocked(uint64_t now) {
    const auto ctx = CurrentUiContext();
    if (ctx != game::UiContext::FmvSkippable || g_editing) {
        g_fmv_progress = 0.0f;
        g_fmv_last_ms = now;
        g_fmv_skip_fired = false;
        return;
    }
    const uint64_t dt_ms = (g_fmv_last_ms != 0 && now >= g_fmv_last_ms) ? std::min<uint64_t>(now - g_fmv_last_ms, 100u) : 16u;
    g_fmv_last_ms = now;

    bool holding = false;
    for (const Finger& f : g_fingers) {
        if (f.role == Role::FmvHold) {
            holding = true;
            break;
        }
    }
    if (holding) {
        g_fmv_progress = std::min(1.0f, g_fmv_progress + float(dt_ms) / kFmvHoldSkipMs);
        if (g_fmv_progress >= 1.0f && !g_fmv_skip_fired) {
            g_fmv_skip_fired = true;
            g_fmv_skip_pulse_until = now + kFmvSkipPulseMs;
            LOGI("[OK] touch: FMV hold-to-skip triggered");
        }
    } else {
        g_fmv_progress = std::max(0.0f, g_fmv_progress - float(dt_ms) / 220.0f);
    }
}

}  // namespace

bool OnEvent(const SDL_Event& e) {
    switch (e.type) {
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
            KeyEvent(e.key);
            return true;
        case SDL_EVENT_TEXT_INPUT:
            for (const char* c = e.text.text; c && *c; c++)
                if (uint8_t(*c) < 0x80 && *c >= ' ') input::OnChar(unsigned(*c));
            return true;
        case SDL_EVENT_MOUSE_MOTION:
            if (e.motion.which == SDL_TOUCH_MOUSEID) return true;
            input::g_keyboard = true;
            input::g_touch = false;
            if (input::Active() && input::Play()) AddLook(e.motion.xrel, e.motion.yrel);
            return true;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (e.button.which == SDL_TOUCH_MOUSEID) return true;
            if (int vk = input::VkOfButton(e.button.button)) input::g_down[size_t(vk)] = e.button.down;
            input::g_keyboard = true;
            input::g_touch = false;
            return true;
        case SDL_EVENT_MOUSE_WHEEL:
            if (e.wheel.y != 0) {
                int notch = e.wheel.y > 0 ? 1 : -1;
                input::g_wheel_ms[notch < 0] = NowMs();
                if (input::Active() && !input::Play() && !input::KeyCaptureUp()) input::g_wheel += notch;
            }
            return true;
        case SDL_EVENT_FINGER_DOWN:
        case SDL_EVENT_FINGER_MOTION:
        case SDL_EVENT_FINGER_UP:
        case SDL_EVENT_FINGER_CANCELED: {
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

// Translate MC3D's live remapped (dev, key) binding directly into Xbox pad state (verified from x86_recomp.7.cpp)
void ApplyFallbackPress(input::Pad& pad, int p) {
    if (p == kSteerLeft) {
        pad.lx = int16_t(-32767);
    } else if (p == kSteerRight) {
        pad.lx = int16_t(32767);
    } else if (p >= 0 && p < kBit) {
        pad.analog[p] = 0xFF;
    } else if (p >= kBit) {
        pad.buttons |= uint16_t(1u << (p - kBit));
    }
}

void ApplyRemappedAction(input::Pad& pad, game::GameAction act, int dir_sign, int fallback_press) {
    if (act >= game::GameAction::Count) {
        ApplyFallbackPress(pad, fallback_press);
        return;
    }
    const auto& slot = game::Telemetry().bindings[static_cast<size_t>(act)];
    const uint32_t dev = slot.dev.load(std::memory_order_relaxed);
    const uint32_t key = slot.key.load(std::memory_order_relaxed);

    if (act == game::GameAction::SteerAxis) {
        if (dir_sign == 0) return;
        const int16_t val = (dir_sign < 0) ? int16_t(-32767) : int16_t(32767);
        if (dev == 0x09u) {
            if (key == 0x0000u) { pad.lx = val; return; }            // Left Analog X
            if (key == 0x0002u) { pad.rx = val; return; }            // Right Analog X
            if (key == 0x000Cu) {                                    // D-Pad Left / Right
                pad.buttons |= uint16_t(dir_sign < 0 ? (1u << 2) : (1u << 3));
                return;
            }
        }
        pad.lx = val;
        return;
    }

    if (dev == 0x06u && key != 0u) {
        // MC3D internal gamepad button bitmask -> exact Xbox XINPUT_GAMEPAD mapping
        if (key & 0x0001u) pad.analog[kAnalogWhite] = 0xFF;  // L2 -> White
        if (key & 0x0002u) pad.analog[kAnalogBlack] = 0xFF;  // R2 -> Black
        if (key & 0x0004u) pad.analog[kAnalogLT] = 0xFF;     // L1 -> LT
        if (key & 0x0008u) pad.analog[kAnalogRT] = 0xFF;     // R1 -> RT
        if (key & 0x0010u) pad.analog[kAnalogY] = 0xFF;      // Triangle -> Y
        if (key & 0x0020u) pad.analog[kAnalogB] = 0xFF;      // Circle -> B
        if (key & 0x0040u) pad.analog[kAnalogA] = 0xFF;      // XButton -> A
        if (key & 0x0080u) pad.analog[kAnalogX] = 0xFF;      // Square -> X
        if (key & 0x0100u) pad.buttons |= uint16_t(1u << 5); // Select -> Back
        if (key & 0x0200u) pad.buttons |= uint16_t(1u << 6); // L3
        if (key & 0x0400u) pad.buttons |= uint16_t(1u << 7); // R3
        if (key & 0x0800u) pad.buttons |= uint16_t(1u << 4); // Start
        if (key & 0x1000u) pad.buttons |= uint16_t(1u << 0); // D-Pad Up
        if (key & 0x2000u) pad.buttons |= uint16_t(1u << 3); // D-Pad Right
        if (key & 0x4000u) pad.buttons |= uint16_t(1u << 1); // D-Pad Down
        if (key & 0x8000u) pad.buttons |= uint16_t(1u << 2); // D-Pad Left
        return;
    }
    if (dev == 0x08u) {
        if (key == 0x0008u) { pad.analog[kAnalogLT] = 0xFF; return; }
        if (key == 0x0009u) { pad.analog[kAnalogRT] = 0xFF; return; }
    }
    if (dev == 0x09u) {
        switch (key) {
            case 0x0004u: pad.ly = int16_t(32767); return;   // L Analog Up
            case 0x0005u: pad.ly = int16_t(-32767); return;  // L Analog Down
            case 0x0006u: pad.lx = int16_t(-32767); return;  // L Analog Left
            case 0x0007u: pad.lx = int16_t(32767); return;   // L Analog Right
            case 0x0008u: pad.ry = int16_t(32767); return;   // R Analog Up
            case 0x0009u: pad.ry = int16_t(-32767); return;  // R Analog Down
            case 0x000Au: pad.rx = int16_t(-32767); return;  // R Analog Left
            case 0x000Bu: pad.rx = int16_t(32767); return;   // R Analog Right
            default: break;
        }
    }
    ApplyFallbackPress(pad, fallback_press);
}

void ApplyRemappedSteer(input::Pad& pad, float steer_f) {
    if (std::fabs(steer_f) < 0.02f) return;
    const float clamped = std::clamp(steer_f, -1.0f, 1.0f);
    const int16_t val = int16_t(std::lround(clamped * 32767.0f));
    const auto& slot = game::Telemetry().bindings[static_cast<size_t>(game::GameAction::SteerAxis)];
    const uint32_t dev = slot.dev.load(std::memory_order_relaxed);
    const uint32_t key = slot.key.load(std::memory_order_relaxed);
    if (dev == 0x09u) {
        if (key == 0x0000u) { pad.lx = val; return; }            // Left Analog X
        if (key == 0x0002u) { pad.rx = val; return; }            // Right Analog X
        if (key == 0x000Cu) {                                    // D-Pad Left / Right
            if (std::fabs(clamped) >= 0.18f) {
                pad.buttons |= uint16_t(clamped < 0.0f ? (1u << 2) : (1u << 3));
            }
            return;
        }
    }
    pad.lx = val;
}

void Apply(int port, input::Pad& pad) {
    if (port != 0 || g_editing) return;
    std::lock_guard lock(g_lock);
    const uint64_t now = NowMs();
    UpdateFmvHoldLocked(now);
    Recount();
    bool used = false;
    bool gas_held = false;
    int steer = 0;
    const auto ctx = CurrentUiContext();

    for (size_t i = 0; i < kButtonCount; i++) {
        if (!g_pressed[i]) continue;
        const int p = kButtons[i].press;
        used = true;
        if (kButtons[i].action == game::GameAction::Gas) gas_held = true;
        if (p == kSteerLeft) {
            steer -= 1;
        } else if (p == kSteerRight) {
            steer += 1;
        } else if (p == kBurnout) {
            // Burnout: simultaneously hold Gas + Handbrake (synced with any in-game remaps!)
            ApplyRemappedAction(pad, game::GameAction::Gas, 0, kAnalogRT);
            ApplyRemappedAction(pad, game::GameAction::Handbrake, 0, kAnalogA);
        } else if (p != kSteerStick) {
            ApplyRemappedAction(pad, kButtons[i].action, 0, p);
        }
    }

    // QoL: Auto Burnout — when holding Gas during pre-race 3-2-1 countdown, automatically hold Handbrake
    // for a perfect burnout boost launch, releasing Handbrake the instant GO! fires.
    if (ctx == game::UiContext::Gameplay && gas_held && game::Opt().auto_burnout &&
        game::Telemetry().race_countdown.load(std::memory_order_relaxed)) {
        ApplyRemappedAction(pad, game::GameAction::Handbrake, 0, kAnalogA);
    }

    // Steering: digital Left/Right buttons OR proportional NFSU2-style analog thumbstick
    const float steer_f = (steer != 0) ? float(steer) : (g_analog_stick ? g_stick_x : 0.0f);
    if (std::fabs(steer_f) >= 0.02f) {
        ApplyRemappedSteer(pad, steer_f);
        used = true;
    }
    if (g_analog_stick && ctx == game::UiContext::Gameplay && std::fabs(g_stick_y) >= 0.18f) {
        // Thumbstick up/down pitches motorcycle weight (tuck forward / wheelie back)
        pad.ly = int16_t(std::lround(std::clamp(-g_stick_y, -1.0f, 1.0f) * 32767.0f));
        used = true;
    }

    // Bikes automatically engage Weight Transfer whenever steering for sharp corner carving!
    if (ctx == game::UiContext::Gameplay &&
        game::Telemetry().is_bike.load(std::memory_order_relaxed) &&
        std::fabs(steer_f) >= 0.12f) {
        ApplyRemappedAction(pad, game::GameAction::WeightTransfer, 0, kAnalogB);
    }

    // #2 UX: 1-finger Garage orbit camera drag -> Right Stick (rx / ry)
    if (ctx == game::UiContext::Garage &&
        (std::fabs(g_orbit_x) > 0.02f || std::fabs(g_orbit_y) > 0.02f)) {
        pad.rx = int16_t(std::lround(std::clamp(g_orbit_x, -1.0f, 1.0f) * 32767.0f));
        pad.ry = int16_t(std::lround(std::clamp(g_orbit_y, -1.0f, 1.0f) * 32767.0f));
        used = true;
    }

    // Swipe gestures: during Gameplay, Radio Left/Right follow remapped MUSIC_DOWN / MUSIC_UP!
    for (int bit = 0; bit < 4; ++bit) {
        if (now < g_pulse_until[bit]) {
            used = true;
            if (ctx == game::UiContext::Gameplay && bit == 2) {
                ApplyRemappedAction(pad, game::GameAction::RadioPrev, 0, kBit + 2);
            } else if (ctx == game::UiContext::Gameplay && bit == 3) {
                ApplyRemappedAction(pad, game::GameAction::RadioNext, 0, kBit + 3);
            } else {
                pad.buttons |= uint16_t(1u << bit);
            }
        }
    }

    // FMV hold-anywhere skip completed -> fire Start + A to skip movie cleanly
    if (now < g_fmv_skip_pulse_until) {
        pad.buttons |= uint16_t(1u << 4);  // Start
        pad.analog[kAnalogA] = 0xFF;       // A
        used = true;
    }

    if (now < g_back_until) {
        used = true;
        if (IsMenuLike(ctx)) pad.analog[kAnalogB] = 0xFF;
        else ApplyRemappedAction(pad, game::GameAction::Pause, 0, kBit + 4);
    }

    if (used) pad.connected = true;
}

bool Shown() {
    std::lock_guard lock(g_lock);
    return CurrentFadeAlphaLocked(NowMs()) > 0.005f;
}

void BuildOverlay(std::vector<Quad>& out) {
    std::lock_guard lock(g_lock);
    EnsureLayout();
    const uint64_t now = NowMs();
    UpdateFmvHoldLocked(now);

    const bool editing = g_editing;
    const float fade_alpha = CurrentFadeAlphaLocked(now);
    if (!editing && fade_alpha <= 0.004f) return;

    const float w = Width();
    const float h = Height();
    const float th = std::max(2.0f, h / 360.0f);
    const uint32_t skin = std::min<uint32_t>(g_skin, kSkins - 1);
    const bool atlas_ok = g_skin_ready.load(std::memory_order_relaxed);

    // 1. Editor backdrop + 5% snap grid + live magnetic symmetry alignment guides (zero ImGui!)
    if (editing) {
        EmitPanel(out, 0, 0, w, h, 0.03f, 0.04f, 0.06f, 0.56f, 0.0f);
        if (g_snap_grid) {
            for (int step = 1; step < 20; ++step) {
                const float f = float(step) * 0.05f;
                const bool major = (step == 5 || step == 10 || step == 15);
                const float a = major ? 0.13f : 0.045f;
                EmitPanel(out, w * f - 0.5f, 0, w * f + 0.5f, h, 1.0f, 1.0f, 1.0f, a, 0.0f);
                EmitPanel(out, 0, h * f - 0.5f, w, h * f + 0.5f, 1.0f, 1.0f, 1.0f, a, 0.0f);
            }
        } else {
            for (float f : {0.25f, 0.50f, 0.75f}) {
                EmitPanel(out, w * f - 0.5f, 0, w * f + 0.5f, h, 1.0f, 1.0f, 1.0f, 0.09f, 0.0f);
                EmitPanel(out, 0, h * f - 0.5f, w, h * f + 0.5f, 1.0f, 1.0f, 1.0f, 0.09f, 0.0f);
            }
        }
        // Live glowing alignment guides when magnetically snapped during drag
        if (g_guide_x >= 0.0f) {
            EmitPanel(out, w * g_guide_x - 1.0f, 0, w * g_guide_x + 1.0f, h, 0.25f, 0.92f, 1.0f, 0.75f, 0.0f);
        }
        if (g_guide_mirror_x >= 0.0f) {
            EmitPanel(out, w * g_guide_mirror_x - 1.0f, 0, w * g_guide_mirror_x + 1.0f, h, 1.0f, 0.80f, 0.28f, 0.55f, 0.0f);
        }
        if (g_guide_y >= 0.0f) {
            EmitPanel(out, 0, h * g_guide_y - 1.0f, w, h * g_guide_y + 1.0f, 0.25f, 0.92f, 1.0f, 0.75f, 0.0f);
        }
    }

    // 2. FMV Skippable Cinema Mode: ONLY start.svg + animated circular hold-anywhere progress ring!
    const auto ctx = CurrentUiContext();
    if (!editing && ctx == game::UiContext::FmvSkippable) {
        const float aspect = std::clamp(w / std::max(h, 1.0f), 1.33f, 2.55f);
        const float r_share = 0.072f;
        const float btn_r = r_share * h;
        const float ring_r = btn_r * 1.24f;
        const float cx = (1.0f - 0.045f - (r_share * 1.24f) / aspect) * w;
        const float cy = (1.0f - 0.115f - r_share) * h;

        const float prog = g_fmv_progress;
        const float pulse = 1.0f + prog * 0.09f;
        const float vis_a = fade_alpha * g_opacity;

        // Circular progress ring (subtle track at rest, bright white/gold clockwise fill when holding anywhere!)
        const float track_a = (prog > 0.01f ? 0.35f : 0.22f) * vis_a;
        const float active_a = 0.98f * vis_a;
        EmitRing(out, cx, cy, ring_r * pulse, std::max(4.0f, h * 0.0085f), prog,
                 1.0f, 0.88f, 0.38f, active_a,
                 1.0f, 1.0f, 1.0f, track_a);

        // start.svg icon inside the ring
        const AtlasUv& uv = g_atlas_uv[skin][static_cast<size_t>(SvgIcon::Start)];
        const float ir = btn_r * 0.92f * pulse;
        if (atlas_ok && uv.valid) {
            out.push_back(Quad{cx - ir, cy - ir, cx + ir, cy + ir,
                               uv.u0, uv.v0, uv.u1, uv.v1,
                               1.0f, 1.0f, 1.0f, std::min(1.0f, vis_a * (prog > 0.01f ? 1.15f : 0.95f)),
                               0, 0, 0, 0, 0, 0, 0u});
        } else {
            EmitPanel(out, cx - ir, cy - ir, cx + ir, cy + ir,
                      0.10f, 0.10f, 0.14f, 0.65f * vis_a, ir, th, 1.0f, 1.0f, 1.0f, 0.85f * vis_a);
        }

        // Crisp Roboto "Hold to Skip" label beneath ring
        EmitTextCentered(out, cx, cy + ring_r + h * 0.032f, h * 0.030f,
                         1.0f, 1.0f, 1.0f, 0.88f * vis_a, "Hold to Skip");
        return;
    }

    const EditorTheme theme = EditorThemeForSkin(skin, th);

    // 3. Draw active touch buttons (Gameplay, Main Menu, Garage, or Editor)
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
        const float vis = (hidden ? 0.32f : 1.0f) * g_opacity * fade_alpha;
        const float press_scale = on ? 0.94f : 1.0f;

        const float dx0 = cx - bw * 0.5f * press_scale;
        const float dy0 = cy - bh * 0.5f * press_scale;
        const float dx1 = cx + bw * 0.5f * press_scale;
        const float dy1 = cy + bh * 0.5f * press_scale;

        const SvgIcon icon = ActiveIconForButton(i);
        const bool stick_active = !editing && i == kIdxSteerStick &&
                                  (std::fabs(g_stick_raw_x) > 0.01f || std::fabs(g_stick_raw_y) > 0.01f);

        if (i == kIdxSteerStick) {
            // Native SDF Analog Steering Thumbstick matching active Button Style + NFSU2 lime active glow
            const float rad = std::min(bw, bh) * 0.5f;
            const float fill_r = (skin == kFilledWhite) ? 0.96f : (skin == kFilledDark ? 0.08f : 0.04f);
            const float fill_g = (skin == kFilledWhite) ? 0.97f : (skin == kFilledDark ? 0.09f : 0.05f);
            const float fill_b = (skin == kFilledWhite) ? 0.99f : (skin == kFilledDark ? 0.12f : 0.07f);
            const float fill_a = (skin == kOutline ? 0.22f : 0.42f) * vis;
            const float rim_r = stick_active ? 0.784f : 1.0f;
            const float rim_g = stick_active ? 0.941f : 1.0f;
            const float rim_b = stick_active ? 0.533f : 1.0f;
            const float rim_a = (stick_active ? 0.98f : 0.86f) * vis;
            EmitPanel(out, x0, y0, x1, y1, fill_r, fill_g, fill_b, fill_a, rad,
                      stick_active ? th * 1.45f : th * 1.15f, rim_r, rim_g, rim_b, rim_a);

            // Subtle directional guide chevrons inside the ring
            const float chev_a = (skin == kFilledWhite ? 0.55f : 0.50f) * vis;
            const float chev_c = (skin == kFilledWhite) ? 0.12f : 1.0f;
            EmitTextCentered(out, cx - rad * 0.68f, cy, rad * 0.34f, chev_c, chev_c, chev_c, chev_a, "<");
            EmitTextCentered(out, cx + rad * 0.68f, cy, rad * 0.34f, chev_c, chev_c, chev_c, chev_a, ">");

            // Animated inner thumb knob
            const float knob_r = rad * 0.42f;
            const float kx = cx + (editing ? 0.0f : g_stick_raw_x * rad * 0.50f);
            const float ky = cy + (editing ? 0.0f : g_stick_raw_y * rad * 0.50f);
            const float kf_r = (skin == kFilledDark) ? 0.14f : 0.96f;
            const float kf_g = (skin == kFilledDark) ? 0.16f : 0.97f;
            const float kf_b = (skin == kFilledDark) ? 0.20f : 1.00f;
            const float kf_a = (skin == kOutline ? (stick_active ? 0.45f : 0.26f) : 0.92f) * vis;
            EmitPanel(out, kx - knob_r, ky - knob_r, kx + knob_r, ky + knob_r,
                      kf_r, kf_g, kf_b, kf_a, knob_r,
                      th * 1.25f, rim_r, rim_g, rim_b, std::min(1.0f, vis * 1.05f));
        } else {
            // #1 UX: Live Auto-Burnout pulsing golden-amber launch cue on Gas pedal during 3-2-1 countdown!
            const bool launch_cue = !editing && i == kIdxGas && ctx == game::UiContext::Gameplay &&
                                    game::Opt().auto_burnout &&
                                    game::Telemetry().race_countdown.load(std::memory_order_relaxed);
            if (launch_cue) {
                const float pulse = 0.5f + 0.5f * std::sin(float(now % 6283u) * 0.008f);
                const float glow_pad = th * (on ? 4.2f : (2.4f + pulse * 1.6f));
                const float gr = on ? 0.784f : 1.000f;
                const float gg = on ? 0.941f : 0.720f;
                const float gb = on ? 0.533f : 0.180f;
                const float ga = (on ? 0.96f : (0.52f + pulse * 0.40f)) * fade_alpha;
                EmitPanel(out, dx0 - glow_pad, dy0 - glow_pad, dx1 + glow_pad, dy1 + glow_pad,
                          gr, gg, gb, (on ? 0.26f : (0.10f + pulse * 0.12f)) * fade_alpha,
                          bw * 0.28f + glow_pad * 0.4f, th * 1.65f, gr, gg, gb, ga);
            }

            const AtlasUv& uv = g_atlas_uv[skin][static_cast<size_t>(icon)];
            if (atlas_ok && uv.valid) {
                float tr = 1.0f, tg = 1.0f, tb = 1.0f, ta = std::min(1.0f, vis * (on ? 1.15f : 0.92f));
                if (kButtons[i].when == When::PlayTurbo && !editing) {
                    tr = on ? 1.0f : 0.48f;
                    tg = on ? 0.94f : 0.92f;
                    tb = on ? 0.72f : 1.0f;
                    ta = std::min(1.0f, vis * 1.10f);
                } else if (launch_cue) {
                    tr = 1.0f;
                    tg = on ? 0.98f : 0.90f;
                    tb = on ? 0.65f : 0.55f;
                    ta = std::min(1.0f, vis * 1.15f);
                }
                out.push_back(Quad{dx0, dy0, dx1, dy1, uv.u0, uv.v0, uv.u1, uv.v1,
                                   tr, tg, tb, ta, 0, 0, 0, 0, 0, 0, 0u});
            } else {
                EmitPanel(out, dx0, dy0, dx1, dy1,
                          on ? 0.9f : 0.08f, on ? 0.9f : 0.09f, on ? 0.95f : 0.12f, (on ? 0.42f : 0.55f) * vis,
                          std::min(bw, bh) * 0.25f, th, 1.0f, 1.0f, 1.0f, 0.8f * vis);
            }

            if (launch_cue) {
                const float badge_w = std::max(bw * 1.18f, h * 0.165f);
                const float badge_h = h * 0.044f;
                const float badge_cy = dy0 + h * 0.010f;
                const float br = on ? 0.784f : 1.000f;
                const float bg = on ? 0.941f : 0.745f;
                const float bb = on ? 0.533f : 0.220f;
                EmitPanel(out, cx - badge_w * 0.5f, badge_cy - badge_h * 0.5f,
                          cx + badge_w * 0.5f, badge_cy + badge_h * 0.5f,
                          0.06f, 0.08f, 0.11f, 0.88f * fade_alpha, badge_h * 0.48f,
                          th * 1.05f, br, bg, bb, 0.96f * fade_alpha);
                EmitTextCentered(out, cx, badge_cy, h * 0.025f,
                                 br, bg, bb, 0.98f * fade_alpha, on ? "BURNOUT" : "LAUNCH");
            }
        }

        if (editing) {
            if (int(i) == g_selected) {
                const float pad = th * 3.0f;
                // NFSU2-style lime selection outline (#C8F088)
                EmitPanel(out, x0 - pad, y0 - pad, x1 + pad, y1 + pad,
                          0.784f, 0.941f, 0.533f, 0.14f, std::min(bw, bh) * 0.28f + pad * 0.5f,
                          th * 1.45f, 0.784f, 0.941f, 0.533f, 0.96f);
            } else if (hidden) {
                EmitPanel(out, x0, y0, x1, y1,
                          0.95f, 0.28f, 0.24f, 0.14f, std::min(bw, bh) * 0.24f,
                          th * 0.9f, 0.98f, 0.36f, 0.32f, 0.78f);
            }
            EmitTextCentered(out, cx, y1 + h * 0.028f, h * 0.030f,
                             1.0f, 1.0f, 1.0f, hidden ? 0.50f : 0.94f, kButtons[i].label);
        }
    }

    // 4. Radio swipe subtle capsule toast during gameplay (matches active Button Style + Roboto)
    if (!editing && ctx == game::UiContext::Gameplay && now < g_radio_toast_until) {
        const float remain = float(g_radio_toast_until - now) / float(kToastMs);
        const float alpha = std::clamp(remain * 1.5f, 0.0f, 1.0f) * fade_alpha;
        const char* msg = (g_radio_toast_dir > 0) ? "Radio  >" : "<  Radio";
        const float ty = h * 0.88f;
        const float pw = h * 0.24f;
        const float ph = h * 0.064f;
        EmitPanel(out, w * 0.5f - pw * 0.5f, ty - ph * 0.5f, w * 0.5f + pw * 0.5f, ty + ph * 0.5f,
                  theme.pill_r, theme.pill_g, theme.pill_b, theme.pill_a * alpha, ph * 0.48f,
                  theme.pill_bw, theme.pill_br, theme.pill_bg, theme.pill_bb, theme.pill_ba * alpha);
        EmitTextCentered(out, w * 0.5f, ty, h * 0.032f,
                         theme.pill_tr, theme.pill_tg, theme.pill_tb, 0.96f * alpha, msg);
    }

    // 5. NFSU2-Style Mobile-Sized Floating Draggable Two-Row Layout Editor Card (Zero ImGui, Google Roboto)
    if (editing) {
        const EditorCard c = ComputeEditorCard();
        const float card_r = h * 0.036f;
        const float btn_r = h * 0.024f;
        const float border_w = std::max(2.0f, th * 1.05f);

        // Dark translucent slate card (#DD101620), lime border (#C8F088) when dragging
        EmitPanel(out, c.card[0], c.card[1], c.card[2], c.card[3],
                  0.063f, 0.086f, 0.125f, 0.89f, card_r,
                  g_bar_dragging ? th * 1.65f : border_w,
                  g_bar_dragging ? 0.784f : 1.0f,
                  g_bar_dragging ? 0.941f : 1.0f,
                  g_bar_dragging ? 0.533f : 1.0f,
                  g_bar_dragging ? 0.96f : 0.38f);

        // Left-side draggable hamburger grip handle (3 bold mobile horizontal bars)
        const float gx = 0.5f * (c.grip[0] + c.grip[2]);
        const float gy = 0.5f * (c.grip[1] + c.grip[3]);
        const float bar_hw = (c.grip[2] - c.grip[0]) * 0.26f;
        const float bar_hh = std::max(2.0f, h * 0.0040f);
        const float bar_sp = h * 0.022f;
        const float gr = g_bar_dragging ? 0.784f : 1.0f;
        const float gg = g_bar_dragging ? 0.941f : 1.0f;
        const float gb = g_bar_dragging ? 0.533f : 1.0f;
        const float ga = g_bar_dragging ? 0.95f : 0.62f;
        for (int k = -1; k <= 1; ++k) {
            const float ly = gy + float(k) * bar_sp;
            EmitPanel(out, gx - bar_hw, ly - bar_hh, gx + bar_hw, ly + bar_hh,
                      gr, gg, gb, ga, bar_hh);
        }

        const bool has_sel = (g_selected >= 0 && size_t(g_selected) < kButtonCount);
        const bool can_hide = Hideable(g_selected);
        const bool sel_hidden = has_sel && g_layout[size_t(g_selected)].hidden;

        auto draw_nfsu2_btn = [&](int idx, const char* label, float txt_h,
                                  float fr, float fg, float fb, float fa,
                                  float sr, float sg, float sb, float sa,
                                  float tr, float tg, float tb, float ta) {
            const float* r = c.btns[idx];
            EmitPanel(out, r[0], r[1], r[2], r[3], fr, fg, fb, fa, btn_r,
                      border_w, sr, sg, sb, sa);
            EmitTextCentered(out, 0.5f * (r[0] + r[2]), 0.5f * (r[1] + r[3]),
                             txt_h, tr, tg, tb, ta, label);
        };

        const float lbl_h = h * 0.038f;
        const float sym_h = h * 0.058f;

        // Row 1: RESET | - | + | SAVE (Lime) | CANCEL
        draw_nfsu2_btn(kEdReset, "RESET", lbl_h,
                       0.094f, 0.125f, 0.157f, 0.90f,  1.0f, 1.0f, 1.0f, 0.36f,  0.90f, 0.91f, 0.93f, 0.96f);
        draw_nfsu2_btn(kEdMinus, "-", sym_h,
                       0.118f, 0.157f, 0.212f, has_sel ? 0.92f : 0.54f,
                       1.0f, 1.0f, 1.0f, has_sel ? 0.45f : 0.20f,
                       1.0f, 1.0f, 1.0f, has_sel ? 0.98f : 0.42f);
        draw_nfsu2_btn(kEdPlus, "+", sym_h,
                       0.118f, 0.157f, 0.212f, has_sel ? 0.92f : 0.54f,
                       1.0f, 1.0f, 1.0f, has_sel ? 0.45f : 0.20f,
                       1.0f, 1.0f, 1.0f, has_sel ? 0.98f : 0.42f);
        draw_nfsu2_btn(kEdSave, "SAVE", lbl_h,
                       0.094f, 0.188f, 0.110f, 0.94f,  0.533f, 0.878f, 0.376f, 0.78f,  0.784f, 0.941f, 0.533f, 0.98f);
        draw_nfsu2_btn(kEdCancel, "CANCEL", lbl_h,
                       0.094f, 0.125f, 0.157f, 0.90f,  1.0f, 1.0f, 1.0f, 0.36f,  0.90f, 0.91f, 0.93f, 0.96f);

        // Row 2: PLAY/MENU (Sky Blue) | SNAP ON/OFF (Teal) | HIDE/SHOW (Amber/Lime)
        draw_nfsu2_btn(kEdTab, g_edit_menu_tab ? "MENU" : "PLAY", lbl_h,
                       0.086f, 0.145f, 0.204f, 0.92f,  0.353f, 0.784f, 0.980f, 0.72f,  0.478f, 0.847f, 1.000f, 0.98f);
        if (g_snap_grid) {
            draw_nfsu2_btn(kEdSnap, "SNAP ON", lbl_h,
                           0.086f, 0.176f, 0.157f, 0.92f,  0.361f, 0.910f, 0.753f, 0.72f,  0.486f, 0.973f, 0.816f, 0.98f);
        } else {
            draw_nfsu2_btn(kEdSnap, "SNAP OFF", lbl_h,
                           0.094f, 0.125f, 0.157f, 0.84f,  1.0f, 1.0f, 1.0f, 0.30f,  0.78f, 0.80f, 0.84f, 0.90f);
        }
        if (can_hide) {
            if (sel_hidden) {
                draw_nfsu2_btn(kEdHide, "SHOW", lbl_h,
                               0.094f, 0.188f, 0.110f, 0.92f,  0.533f, 0.878f, 0.376f, 0.74f,  0.784f, 0.941f, 0.533f, 0.98f);
            } else {
                draw_nfsu2_btn(kEdHide, "HIDE", lbl_h,
                               0.157f, 0.125f, 0.094f, 0.92f,  1.000f, 0.718f, 0.302f, 0.74f,  1.000f, 0.718f, 0.302f, 0.98f);
            }
        } else {
            draw_nfsu2_btn(kEdHide, "HIDE", lbl_h,
                           0.094f, 0.125f, 0.157f, 0.52f,  1.0f, 1.0f, 1.0f, 0.20f,  0.70f, 0.72f, 0.76f, 0.44f);
        }

        // Readable pill status badge beneath (or above) card
        char status[80];
        if (has_sel) {
            const int pct = int(std::lround(g_layout[size_t(g_selected)].r * 200.0f));
            std::snprintf(status, sizeof(status), "%s  -  %d%%  (Pinch or +/-)",
                          kButtons[size_t(g_selected)].label, pct);
        } else {
            std::snprintf(status, sizeof(status), "Drag controls to move  |  Drag handle to move bar");
        }
        const float st_cx = 0.5f * (c.card[0] + c.card[2]);
        const float st_h = h * 0.056f;
        const float st_w = std::min(c.card[2] - c.card[0], h * 1.18f);
        EmitPanel(out, st_cx - st_w * 0.5f, c.status_cy - st_h * 0.5f,
                  st_cx + st_w * 0.5f, c.status_cy + st_h * 0.5f,
                  0.063f, 0.086f, 0.125f, 0.78f, st_h * 0.48f,
                  th * 0.75f, has_sel ? 0.784f : 1.0f, has_sel ? 0.941f : 1.0f, has_sel ? 0.533f : 1.0f, 0.32f);
        EmitTextCentered(out, st_cx, c.status_cy, h * 0.031f,
                         has_sel ? 0.784f : 0.95f, has_sel ? 0.941f : 0.97f, has_sel ? 0.533f : 1.00f,
                         0.95f, status);
    }
}

bool LoadSkin(std::vector<uint8_t>& rgba, uint32_t& out_w, uint32_t& out_h) {
    constexpr uint32_t kAtlasW = 2048;
    constexpr uint32_t kAtlasH = 3072;
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
            if (cur_y + spec.cell_h + kPad > 2420u) {
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

    // Bake anti-aliased proportional Google Roboto font into bottom region (y = 2432..3008)
    BakeRobotoIntoAtlas(rgba, kAtlasW, kAtlasH, 2432u);

    out_w = kAtlasW;
    out_h = kAtlasH;
    LOGI("[OK] touch: rasterized %d SVG icons + Roboto font into %ux%u native Vulkan atlas",
         loaded_count, out_w, out_h);
    return loaded_count > 0;
}

void SetSkinReady(bool ready) {
    g_skin_ready.store(ready, std::memory_order_relaxed);
}

// ============================================================================
// Options API (src/common/menu.cpp Touchscreen Options)
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

const char* StickModeName() {
    std::lock_guard lock(g_lock);
    EnsureLayout();
    return g_analog_stick ? "Analog" : "Buttons";
}

void StepStickMode(int) {
    std::lock_guard lock(g_lock);
    EnsureLayout();
    g_analog_stick = !g_analog_stick;
    g_stick_raw_x = g_stick_raw_y = g_stick_x = g_stick_y = 0.0f;
    SaveLayout();
}

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
    g_layout = ComputeDefaultLayout(Width(), Height());
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

void PadUsed() {
    input::g_touch = false;
    std::lock_guard lock(g_lock);
    StartFadeLocked(0.0f, NowMs());  // Smooth 200ms fade-out when external controller pressed!
}

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
    return std::any_of(touch::g_fingers.begin(), touch::g_fingers.end(), [](const touch::Finger& f) {
        return f.role == touch::Role::None;
    });
}

bool TouchActive() { return g_touch; }

std::string TouchName(int button) {
    std::lock_guard lock(touch::g_lock);
    for (size_t i = 0; i < touch::kButtonCount; i++) {
        const touch::Button& b = touch::kButtons[i];
        if (b.press != button || touch::g_layout[i].hidden) continue;
        return b.label;
    }
    return {};
}

}  // namespace input
