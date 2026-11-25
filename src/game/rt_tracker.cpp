#include "game/rt_tracker.h"

#include <atomic>
#include <mutex>
#include <unordered_set>

#include "kernel/memory.h"

namespace game {
namespace rt_tracker {

namespace {

constexpr size_t kMaxTrackedTargets = 256u;

std::mutex g_rt_mu;
std::unordered_set<uint32_t> g_known_targets;
std::atomic<uint32_t> g_count{0};

}  // namespace

void RecordTargetData(uint32_t data_va) {
    if (!data_va) return;
    std::lock_guard<std::mutex> lk(g_rt_mu);
    if (g_known_targets.size() >= kMaxTrackedTargets) return;
    if (g_known_targets.insert(data_va).second) {
        g_count.store(static_cast<uint32_t>(g_known_targets.size()), std::memory_order_release);
    }
}

void RecordActiveTargets(const gpu::State& s) {
    // gpu::Surface::data holds the physical address (R32(header + 4)), whereas
    // gpu::TextureDesc::data holds mem::VirtualFromPhysical(R32(header + 4)).
    if (s.rt.data) RecordTargetData(mem::VirtualFromPhysical(s.rt.data));
    if (s.zs.data) RecordTargetData(mem::VirtualFromPhysical(s.zs.data));
}

bool IsKnownRenderTarget(uint32_t data_va) {
    if (!data_va || g_count.load(std::memory_order_acquire) == 0u) return false;
    std::lock_guard<std::mutex> lk(g_rt_mu);
    return g_known_targets.find(data_va) != g_known_targets.end();
}

uint32_t TrackedTargetCount() {
    return g_count.load(std::memory_order_acquire);
}

}  // namespace rt_tracker
}  // namespace game
