#pragma once

#include <atomic>
#include <cstdint>

namespace game {

// Live game/UI context used by mobile touch controls to show only minimal relevant buttons.
enum class UiContext : uint8_t {
    MainMenu = 0,  // Frontend, Pause Menu, Options, Save Select -> D-Pad + OK + Back
    Garage = 1,    // Garage / Showroom -> D-Pad Left/Right + OK + Back
    Gameplay = 2,  // Driving / Racing / Cruise -> Tall Steer/Gas/Brake + dynamic NOS/Turbo/Special + Radio swipe
    Loading = 3,   // Loading screen / Transition -> hide driving overlay
};

struct TouchTelemetry {
    std::atomic<uint8_t> ui_context{static_cast<uint8_t>(UiContext::MainMenu)};
    std::atomic<bool> has_nos{false};        // player car has NOS purchased (max_nos > 0)
    std::atomic<bool> turbo_ready{false};    // slipstream turbo filled AND car speed >= 10 km/h (not burnout)
    std::atomic<bool> special_ready{false};  // special ability meter ready/active
    std::atomic<int8_t> special_type{-1};    // 0 = Zone, 1 = Agro, 2 = Roar, -1 = None
    std::atomic<float> speed_kmh{0.0f};      // current car speed in km/h
};

inline TouchTelemetry& Telemetry() {
    static TouchTelemetry t;
    return t;
}

}  // namespace game
