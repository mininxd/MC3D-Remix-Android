#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

// graphics/settings state and FPS overlay helpers (DrawUi / PaceFrame)
namespace app {

bool MenuOpen();
void DrawUi(double fps);
inline constexpr uint32_t kFpsCapRefresh = 1;  // Settings::fps_cap: just under the display's refresh rate
void PaceFrame();  // app::Pace, with kFpsCapRefresh; game thread, after the present

// graphics and option rows exposed to the native in-game options menu
namespace rows {

enum class Kind { Heading, Value, Bind, Action };
struct Row {
    Kind kind;
    int id;  // a bind row's input action
    const char* label;
};
inline constexpr int kDisplayPage = 0;
inline constexpr int kTouchPage = 1;
inline constexpr int kQualityPage = 4;

// 0 display, 1 touch controls, 2 general, 3 mouse and keyboard, 4 quality, 5 credits
const Row* Page(int page, size_t* count);
// the sticks' rows, on the game's control setup page
const Row* Controller(size_t* count);
std::string Value(const Row& row, int step = 0);  // what the row shows, stepped first when step is not zero
void Defaults(int page);
bool Restorable(int page);
bool Enabled(const Row& row);
bool Opens(const Row& row);
bool SliderRatio(int id, float* out_ratio);  // true if id has a continuous range bar (0..1)

}  // namespace rows

}  // namespace app
