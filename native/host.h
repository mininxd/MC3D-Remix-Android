#pragma once
// What the Android host's parts share: the window, the app's lifecycle and the event routing
#include <cstdint>
#include <vector>

union SDL_Event;
struct SDL_Window;

namespace input {
struct Pad;
}

namespace host {

SDL_Window* Window();
// the app is in front with a window to draw to; the renderer and the game wait while it is not
bool Foreground();
// counts the native windows Android gave the app: the renderer makes its surface again when this changes
uint32_t SurfaceEpoch();
// the game thread, before a present: returns once the app is in front again
void WaitForeground();
// the window's size in pixels
void WindowPixels(int& w, int& h);

// Live telemetry snapshot for Display Logs overlay (Off / Minimal / Full / Debug)
struct DisplayLogsFrame {
    float fps = 0.0f;
    float frame_ms = 0.0f;
    float raw_frame_ms = 0.0f;
    float gpu_wait_ms = 0.0f;
    uint32_t draws = 0;
    uint64_t vertices = 0;
    uint32_t culled = 0;
    uint32_t tex_count = 0;
    uint32_t obj_hit_pct = 0;
    uint32_t render_w = 0;
    uint32_t render_h = 0;
};
struct DisplayLogsSnapshot {
    float fps = 0.0f;
    float p1_low_fps = 0.0f;
    float frame_ms = 0.0f;
    float cpu_percent = 0.0f;
    float gpu_percent = 0.0f;
    float temp_c = -1.0f;
    uint32_t draws = 0;
    uint64_t vertices = 0;
    uint32_t culled = 0;
    uint32_t ram_used_mb = 0;
    uint32_t ram_total_mb = 0;
    uint32_t tex_count = 0;
    uint32_t obj_hit_pct = 0;
    uint32_t render_pct = 100;
    uint32_t render_w = 0;
    uint32_t render_h = 0;
    const char* app_version = "1.0";
    const char* build_commit = "";
    const char* vulkan_api = "Vulkan";
    const char* android_ver = "";
    const char* device_name = "";
    const char* cpu_name = "";
    const char* gpu_name = "";
};
void SetGpuInfo(const char* name, uint32_t vk_major, uint32_t vk_minor);
void UpdateDisplayLogs(const DisplayLogsFrame& frame);
DisplayLogsSnapshot GetDisplayLogs();

}  // namespace host

// the render resolution following the frame rate (dynamic_resolution.cpp)
namespace dynres {
uint32_t Percent();  // the render resolution in use, percent of the screen's: RENDER RESOLUTION's while it is off
void Frame();        // vulkan thread, each present
}  // namespace dynres

// controllers through SDL (input.cpp): the main thread hands over their plug events
namespace input {
void OnPadEvent(const SDL_Event& e);
}

// the touch controls and SDL's keyboard and mouse (touch.cpp)
namespace touch {
// Native Vulkan 2D overlay primitive (mode 0 = textured SVG/font atlas, mode 1 = SDF rounded panel/border)
struct Quad {
    float x0, y0, x1, y1;
    float u0, v0, u1, v1;
    float r, g, b, a;
    float radius_px;
    float border_px;
    float br, bg, bb, ba;
    uint32_t mode;
};
// main thread, every input event; true when the touch controls took it
bool OnEvent(const SDL_Event& e);
// the touch controls' buttons and stick onto port 0's pad, before the game reads it
void Apply(int port, input::Pad& pad);
// render thread: emit all visible touch controls / custom editor quads (zero ImGui)
void BuildOverlay(std::vector<Quad>& out);
// any touch on the screen active / fading in
bool Shown();
// main thread, each turn of the event loop: what the input asks of the window (the on-screen keyboard)
void Pump();
// the view turns as the mouse turns it, in mouse counts, while the game is in play
void AddLook(float dx, float dy);
bool LookActive();  // in play, in front, no menu or panel up
bool Firing();      // a fire button of the touch controls, or a controller's right trigger, is held
// an external controller was used: smoothly fades out touch controls over 200ms
void PadUsed();
// the layout's settings in the game's options (src/common/menu.cpp), saved at once
const char* LayoutName();
void StepLayout(int step);
uint32_t ControlsOpacity();  // percent
void StepOpacity(int step);
uint32_t ControlsSize();  // percent of each control's own size
void StepSize(int step);
void EditLayout();  // custom native Vulkan editor over the screen to drag/resize/hide controls
void ResetLayout(); // reset touch control positions, sizes, and visibility to default
const char* SkinName();  // OUTLINE, FILLED WHITE, FILLED DARK
void StepSkin(int step);
const char* StickModeName();
void StepStickMode(int step);
// render thread, once: rasterize SVG skins + crisp UI font into RGBA atlas
bool LoadSkin(std::vector<uint8_t>& rgba, uint32_t& w, uint32_t& h);
void SetSkinReady(bool ready);
}  // namespace touch

// aiming by turning the phone (gyro.cpp), main thread
namespace gyro {
enum Mode : uint32_t { kOff, kAlways, kFiring, kModes };
uint32_t CurrentMode();
const char* ModeName();
void NextMode();  // off, always on, while firing; saved at once
void OnEvent(const SDL_Event& e);
void Pump();  // the sensor opened while a mode wants it, closed otherwise
}  // namespace gyro
