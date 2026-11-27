#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace game {

// Live game/UI context used by mobile touch controls to show only minimal relevant buttons.
enum class UiContext : uint8_t {
    MainMenu = 0,        // Frontend, Pause Menu, Options, Save Select -> D-Pad + OK + Back
    Garage = 1,          // Garage / Showroom -> D-Pad Left/Right + OK + Back
    Gameplay = 2,        // Driving / Racing / Cruise -> Tall Steer/Gas/Brake + dynamic NOS/Turbo/Special + Radio swipe
    Loading = 3,         // Loading screen / Transition -> hide all buttons
    FmvUnskippable = 4,  // Boot Rockstar/Legal intro -> hide all buttons
    FmvSkippable = 5,    // Skippable FMV/Cutscene -> hide all buttons except animated hold-to-skip start.svg
};

// Gameplay actions synced live from MC3D's in-game Controller Configuration / Custom Remap table
// (0x00558848 + active_config * 0x140).
enum class GameAction : uint8_t {
    SteerAxis = 0,  // MC3D action 0x00 STEER_AXIS
    Gas,            // MC3D action 0x02 GAS (fallback 0x04)
    Brake,          // MC3D action 0x03 BRAKE
    Handbrake,      // MC3D action 0x07 HANDBRAKE
    Nitro,          // MC3D action 0x09 NITRO (also activates slipstream turbo)
    LookBack,       // MC3D action 0x0A LOOKBACK
    Camera,         // MC3D action 0x0B CAM_UP
    Map,            // MC3D action 0x0D MAP
    FlashLights,    // MC3D action 0x10 HEADLIGHTS
    Pause,          // MC3D action 0x12 PAUSE
    RadioNext,      // MC3D action 0x14 MUSIC_UP
    RadioPrev,      // MC3D action 0x15 MUSIC_DOWN
    Special,        // MC3D action 0x23 IN_ZONE
    Count,
};

struct RemapSlot {
    std::atomic<uint32_t> dev{0};  // MC3D device type: 0x06=buttons, 0x08=analog trigger, 0x09=axis/dir
    std::atomic<uint32_t> key{0};  // MC3D key/bitmask code
};

struct TouchTelemetry {
    std::atomic<uint8_t> ui_context{static_cast<uint8_t>(UiContext::Loading)};
    std::atomic<bool> has_nos{false};        // player car has NOS purchased (max_nos > 0)
    std::atomic<bool> turbo_ready{false};    // slipstream turbo filled AND car speed >= 10 km/h (not burnout)
    std::atomic<bool> special_ready{false};  // special ability meter ready/active
    std::atomic<int8_t> special_type{-1};    // 0 = Zone, 1 = Agro, 2 = Roar, -1 = None
    std::atomic<float> speed_kmh{0.0f};      // current car speed in km/h
    std::atomic<bool> race_countdown{false}; // true during pre-race 3-2-1 staging countdown before GO!
    std::atomic<bool> garage_vertical{false};// true in Garage when submenu list scrolls Up/Down (depth >= 1)
    std::atomic<uint64_t> fmv_frame_ms{0};   // steady clock ms of last active Bink FMV frame
    std::atomic<bool> fmv_unskippable{true}; // true during unskippable boot Rockstar/Legal movie
    std::array<RemapSlot, static_cast<size_t>(GameAction::Count)> bindings{};
};

inline TouchTelemetry& Telemetry() {
    static TouchTelemetry t;
    return t;
}

void NotifyFmvOpen(const char* name);
void NotifyFmvFrame();
void UpdateTouchTelemetry();

}  // namespace game
