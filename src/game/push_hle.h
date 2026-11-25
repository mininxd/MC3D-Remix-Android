#pragma once
// Clean HLE pushbuffer lifecycle (sub_00416390 BeginPush, sub_00412580 SetPalette)
// and scoped deferred-TSS diff restoration for MC3D's pushbuffer quad drawers.

#include <array>
#include <cstdint>

namespace game {
namespace push_hle {

struct TssDiffSlot {
    bool captured = false;
    bool has_tss = false;
    bool has_ps_modes = false;
    std::array<uint32_t, 128> orig_tss{};
    std::array<uint32_t, 128> post_tss{};
    uint32_t orig_ps_modes = 0;
    uint32_t post_ps_modes = 0;
};

// Held around MC3D's 9 pushbuffer quad callers so sub_00416390 runs ApplyRenderAccuracy()
// right before vertices are pushed into d3d8.cpp's sub_004163B0 (single draw, no double-draw),
// and restores only temporary FF/PS-mode repairs on exit without clobbering caller TSS writes.
class ScopedPushCallerGuard {
public:
    explicit ScopedPushCallerGuard(const char* tag);
    ~ScopedPushCallerGuard();

    ScopedPushCallerGuard(const ScopedPushCallerGuard&) = delete;
    ScopedPushCallerGuard& operator=(const ScopedPushCallerGuard&) = delete;

private:
    const char* prev_tag_ = nullptr;
    TssDiffSlot* prev_slot_ = nullptr;
    TssDiffSlot slot_{};
};

}  // namespace push_hle
}  // namespace game
