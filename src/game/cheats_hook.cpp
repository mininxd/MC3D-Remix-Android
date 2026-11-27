#include "game/cheats_hook.h"

#include "core/log.h"
#include "cpu/x86_ops.h"
#include "game/guest_mem_safe.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>

X86_EXTERN_FUNC(sub_00192350);
X86_EXTERN_FUNC(sub_002693C0);
X86_EXTERN_FUNC(sub_002693D0);
X86_EXTERN_FUNC(sub_002BF570);
X86_EXTERN_FUNC(sub_00326FE0);

X86_EXTERN_FUNC(__imp__sub_001923B0);
X86_EXTERN_FUNC(__imp__sub_00292CD0);
X86_EXTERN_FUNC(__imp__sub_001CF9D0);
X86_EXTERN_FUNC(__imp__sub_00269220);
X86_EXTERN_FUNC(__imp__sub_00029337);
X86_EXTERN_FUNC(__imp__sub_002BD990);
X86_EXTERN_FUNC(__imp__sub_001422D0);
X86_EXTERN_FUNC(__imp__sub_00144C90);
X86_EXTERN_FUNC(__imp__sub_001474A0);
X86_EXTERN_FUNC(__imp__sub_0014E300);

namespace game::cheats {
namespace {

constexpr uint32_t kProfileMgrPtrAddr = 0x00557F0Cu;
constexpr uint32_t kProfileCashOffset = 0xACCu;
constexpr uint32_t kRaceSettingsPtrAddr = 0x0053F2E0u;
constexpr uint32_t kRaceSettingsShadowPtrAddr = 0x0053F2E4u;
constexpr uint32_t kMenuRaceSettingsPtrAddr = 0x0053F2E8u;
constexpr uint32_t kUnlockAllCitiesFlag1 = 0x0053F2ECu;
constexpr uint32_t kUnlockAllCitiesFlag2 = 0x0053F2EDu;
constexpr uint32_t kUnlockAllVehiclesFlag = 0x0053F2EEu;
constexpr uint32_t kTrafficGlobal1 = 0x004B36C8u;
constexpr uint32_t kTrafficGlobal2 = 0x004B36CCu;
constexpr uint32_t kGravityYAddr = 0x005070ECu;
constexpr uint32_t kPoliceMgrPtrAddr = 0x00533E24u;
constexpr uint32_t kGameStateAddr = 0x0053F2D4u;

struct CheatState {
    std::atomic<bool> full_throttle{false};
    std::atomic<bool> secret_dealer{false};
    std::atomic<bool> no_traffic{false};
    std::atomic<bool> low_gravity{false};
    std::atomic<bool> no_cops{false};
    std::atomic<bool> more_cops{false};
    std::atomic<bool> more_more_cops{false};
    std::atomic<bool> zero_cost{false};
    std::atomic<bool> arcade_powerups{false};
    std::atomic<bool> super_copy{false};
    std::atomic<bool> delivery{false};

    bool has_saved_gravity = false;
    float saved_gravity_y = -9.81f;
    bool was_in_race_for_cops = false;
};

CheatState g_state;

uint32_t GetProfilePtr() {
    const uint32_t mgr = safe_mem::Read32(kProfileMgrPtrAddr, 0u);
    if (!mgr) return 0u;
    return safe_mem::Read32(mgr + 0x0Cu, 0u);
}

bool AddCash(int32_t delta) {
    const uint32_t prof = GetProfilePtr();
    if (!prof || !safe_mem::CanAccess(prof + kProfileCashOffset, 4u)) return false;
    const int32_t cur = static_cast<int32_t>(kernel::R32(prof + kProfileCashOffset));
    const int64_t next = static_cast<int64_t>(cur) + static_cast<int64_t>(delta);
    const int32_t clamped = next < 0 ? 0 : (next > 999999999 ? 999999999 : static_cast<int32_t>(next));
    kernel::W32(prof + kProfileCashOffset, static_cast<uint32_t>(clamped));
    return true;
}

uint32_t GetPoliceMgrPtr() {
    const uint32_t outer = safe_mem::Read32(kPoliceMgrPtrAddr, 0u);
    if (!outer) return 0u;
    return safe_mem::Read32(outer + 0x04u, 0u);
}

void ApplyTrafficSettings() {
    if (!g_state.no_traffic.load(std::memory_order_relaxed)) return;
    if (safe_mem::CanAccess(kTrafficGlobal1, 4u)) kernel::W32(kTrafficGlobal1, 0u);
    if (safe_mem::CanAccess(kTrafficGlobal2, 4u)) kernel::W32(kTrafficGlobal2, 0u);
    for (uint32_t ptr_va : {kRaceSettingsPtrAddr, kRaceSettingsShadowPtrAddr}) {
        const uint32_t rs = safe_mem::Read32(ptr_va, 0u);
        if (rs && safe_mem::CanAccess(rs + 0x0Cu, 8u)) {
            kernel::W32(rs + 0x0Cu, 0u);
            kernel::W32(rs + 0x10u, 0u);
        }
    }
}

void ApplyArcadeSettings() {
    if (!g_state.arcade_powerups.load(std::memory_order_relaxed)) return;
    for (uint32_t ptr_va : {kRaceSettingsPtrAddr, kRaceSettingsShadowPtrAddr}) {
        const uint32_t rs = safe_mem::Read32(ptr_va, 0u);
        if (rs && safe_mem::CanAccess(rs + 0x1Cu, 4u)) {
            kernel::W32(rs + 0x1Cu, 1u);
        }
    }
}

void SpawnRandomArcadePowerupsIfEmpty(X86Context& ctx, uint8_t* base) {
    if (!g_state.arcade_powerups.load(std::memory_order_relaxed)) return;
    const uint32_t race_mgr = safe_mem::Read32(0x00557EECu, 0u);
    if (!race_mgr || !safe_mem::CanAccess(race_mgr + 0x80u, 0x10u)) return;
    const uint16_t cur_count = safe_mem::Read16(race_mgr + 0x80u + 0x08u, 0u);
    if (cur_count > 0u) return;

    uint32_t course = safe_mem::Read32(race_mgr + 0xB4u, 0u);
    auto valid_course = [](uint32_t c) {
        if (!c || !safe_mem::CanAccess(c + 0x110u, 4u)) return false;
        const float len = safe_mem::ReadF32(c + 0x110u, 0.0f);
        return len > 1.0f && len < 100000.0f;
    };
    if (!valid_course(course)) {
        course = valid_course(race_mgr) ? race_mgr : 0u;
    }
    if (!course) return;

    const uint32_t orig_esp = ctx.esp;
    const uint32_t orig_eax = ctx.eax;
    const uint32_t orig_ecx = ctx.ecx;
    const uint32_t orig_edx = ctx.edx;

    ctx.ecx = race_mgr + 0x80u;
    X86_PUSH32(0u);
    X86_PUSH32(course);
    X86_PUSH32(10u);  // 10 * 5 = 50 randomized pickups along course (Paint-style)
    X86_PUSH32(0x00029685u);
    sub_00326FE0(ctx, base);

    ctx.esp = orig_esp;
    ctx.eax = orig_eax;
    ctx.ecx = orig_ecx;
    ctx.edx = orig_edx;
    LOGI("[OK] cheats: ARCADE spawned randomized powerups (count=%u)",
         unsigned(safe_mem::Read16(race_mgr + 0x80u + 0x08u, 0u)));
}

void ApplyUnlockFlags() {
    if (g_state.delivery.load(std::memory_order_relaxed)) {
        if (safe_mem::CanAccess(kUnlockAllCitiesFlag1, 1u)) {
            *static_cast<uint8_t*>(kernel::HostPtr(kUnlockAllCitiesFlag1)) = 1u;
        }
        if (safe_mem::CanAccess(kUnlockAllCitiesFlag2, 1u)) {
            *static_cast<uint8_t*>(kernel::HostPtr(kUnlockAllCitiesFlag2)) = 1u;
        }
    }
    if (g_state.secret_dealer.load(std::memory_order_relaxed)) {
        if (safe_mem::CanAccess(kUnlockAllVehiclesFlag, 1u)) {
            *static_cast<uint8_t*>(kernel::HostPtr(kUnlockAllVehiclesFlag)) = 1u;
        }
    }
}

void ApplyLowGravity() {
    if (!safe_mem::CanAccess(kGravityYAddr, 4u)) return;
    if (g_state.low_gravity.load(std::memory_order_relaxed)) {
        if (!g_state.has_saved_gravity) {
            g_state.saved_gravity_y = safe_mem::ReadF32(kGravityYAddr, -9.81f);
            g_state.has_saved_gravity = true;
        }
        const float low_g = g_state.saved_gravity_y * 0.45f;
        uint32_t bits = 0u;
        std::memcpy(&bits, &low_g, sizeof(bits));
        kernel::W32(kGravityYAddr, bits);
    } else if (g_state.has_saved_gravity) {
        uint32_t bits = 0u;
        std::memcpy(&bits, &g_state.saved_gravity_y, sizeof(bits));
        kernel::W32(kGravityYAddr, bits);
        g_state.has_saved_gravity = false;
    }
}

void ApplySuperCopy() {
    if (!g_state.super_copy.load(std::memory_order_relaxed)) return;
    const uint32_t rs = safe_mem::Read32(kRaceSettingsPtrAddr, 0u);
    if (!rs || !safe_mem::CanAccess(rs + 0x4Cu, 4u)) return;
    const int32_t count = static_cast<int32_t>(kernel::R32(rs + 0x4Cu));
    if (count <= 1 || count > 32) return;
    const uint32_t player_info = rs + 0x114u;
    if (!safe_mem::CanAccess(player_info + 0x10u, 8u)) return;
    const uint32_t player_veh_ptr = kernel::R32(player_info + 0x10u);
    const uint32_t player_veh_id = kernel::R32(player_info + 0x14u);
    for (int32_t i = 1; i < count; ++i) {
        const uint32_t ai_info = rs + 0x114u + static_cast<uint32_t>(i) * 0x2Cu;
        if (!safe_mem::CanAccess(ai_info + 0x10u, 8u)) continue;
        kernel::W32(ai_info + 0x10u, player_veh_ptr);
        kernel::W32(ai_info + 0x14u, player_veh_id);
    }
}

void ApplyFullThrottleNos() {
    if (!g_state.full_throttle.load(std::memory_order_relaxed)) return;
    if (safe_mem::Read32(kGameStateAddr, 0u) != 2u) return;
    const uint32_t car_mgr = safe_mem::Read32(0x00558814u, 0u);
    if (!car_mgr) return;
    const uint32_t player_car = safe_mem::Read32(car_mgr + 0x08u, 0u);
    if (!player_car) return;
    const uint32_t veh_p14 = safe_mem::Read32(player_car + 0x14u, 0u);
    if (!veh_p14) return;
    const uint32_t veh_phys = safe_mem::Read32(veh_p14 + 0x08u, 0u);
    if (!veh_phys) return;
    const uint32_t nitro = safe_mem::Read32(veh_phys + 0x180u, 0u);
    if (!nitro || !safe_mem::CanAccess(nitro + 0x0Eu, 6u)) return;
    const int16_t max_nos = static_cast<int16_t>(kernel::R16(nitro + 0x0Eu));
    const int16_t cur_nos = static_cast<int16_t>(kernel::R16(nitro + 0x12u));
    if (max_nos > 0 && cur_nos <= 0) {
        kernel::W16(nitro + 0x12u, static_cast<uint16_t>(max_nos));
    }
}

void ApplyPoliceCheats() {
    const bool no_cops = g_state.no_cops.load(std::memory_order_relaxed);
    const bool more_cops = g_state.more_cops.load(std::memory_order_relaxed);
    const bool more_more_cops = g_state.more_more_cops.load(std::memory_order_relaxed);

    if (more_cops || more_more_cops) {
        for (uint32_t ptr_va : {kRaceSettingsPtrAddr, kRaceSettingsShadowPtrAddr}) {
            const uint32_t rs = safe_mem::Read32(ptr_va, 0u);
            if (rs && safe_mem::CanAccess(rs + 0x18u, 4u)) {
                kernel::W32(rs + 0x18u, 1u);
            }
            if (rs && safe_mem::CanAccess(rs + 0x2Cu, 4u) && kernel::R32(rs + 0x2Cu) < 2u) {
                kernel::W32(rs + 0x2Cu, 2u);
            }
        }
    }

    const uint32_t mgr = GetPoliceMgrPtr();
    const bool in_race = (safe_mem::Read32(kGameStateAddr, 0u) == 2u) && (mgr != 0u);

    if (no_cops && mgr) {
        if (safe_mem::CanAccess(mgr + 0x1254u, 4u)) {
            if (static_cast<int32_t>(kernel::R32(mgr + 0x1254u)) == 0) {
                kernel::W32(mgr + 0x1254u, 0xFFFFFFFFu);
            }
        }
        const uint32_t pursuit_arr = safe_mem::Read32(mgr + 0x65Cu, 0u);
        const uint32_t p0 = pursuit_arr ? safe_mem::Read32(pursuit_arr, 0u) : 0u;
        if (p0 && safe_mem::CanAccess(p0 + 0x44u, 4u)) {
            kernel::W32(p0 + 0x44u, 0u);
        }
    }

    if (in_race && !g_state.was_in_race_for_cops) {
        g_state.was_in_race_for_cops = true;
        if ((more_cops || more_more_cops) && !no_cops) {
            if (safe_mem::CanAccess(mgr + 0x1254u, 4u)) {
                kernel::W32(mgr + 0x1254u, 0u);
            }
            const uint32_t rs = safe_mem::Read32(kRaceSettingsPtrAddr, 0u);
            const int32_t racer_count = (rs && safe_mem::CanAccess(rs + 0x4Cu, 4u))
                                            ? static_cast<int32_t>(kernel::R32(rs + 0x4Cu))
                                            : 1;
            const int32_t max_idx = more_more_cops ? std::min(racer_count, 16) : 1;
            const uint32_t pursuit_arr = safe_mem::Read32(mgr + 0x65Cu, 0u);
            if (pursuit_arr) {
                for (int32_t i = 0; i < max_idx; ++i) {
                    const uint32_t pi = safe_mem::Read32(pursuit_arr + static_cast<uint32_t>(i) * 4u, 0u);
                    if (pi && safe_mem::CanAccess(pi + 0x44u, 4u)) {
                        kernel::W32(pi + 0x44u, 1u);
                    }
                }
            }
        }
    } else if (!in_race) {
        g_state.was_in_race_for_cops = false;
    }
}

bool Toggle(std::atomic<bool>& flag) {
    const bool next = !flag.load(std::memory_order_relaxed);
    flag.store(next, std::memory_order_relaxed);
    return next;
}

bool TryHandleCheatCode(const std::string& raw) {
    std::string code;
    code.reserve(raw.size());
    for (unsigned char c : raw) {
        if (!std::isspace(c)) {
            code.push_back(static_cast<char>(std::toupper(c)));
        }
    }
    if (code.empty()) return false;

    if (code == "AMSSRBLHGSHARK") {
        const bool ok = AddCash(5000);
        LOGI("[OK] cheats: AMSSRBLHGSHARK -> +5000 cash (%s)", ok ? "applied" : "no-profile");
        return true;
    }
    if (code == "FULLTHROTTLE") {
        const bool on = Toggle(g_state.full_throttle);
        LOGI("[OK] cheats: FULLTHROTTLE -> %s", on ? "ON" : "OFF");
        return true;
    }
    if (code == "SECRETDEALER") {
        const bool on = Toggle(g_state.secret_dealer);
        if (on) ApplyUnlockFlags();
        LOGI("[OK] cheats: SECRETDEALER -> %s", on ? "ON" : "OFF");
        return true;
    }
    if (code == "GHOSTTOWN" || code == "GENOSIDA") {
        const bool on = Toggle(g_state.no_traffic);
        if (on) ApplyTrafficSettings();
        LOGI("[OK] cheats: %s -> No Traffic %s", code.c_str(), on ? "ON" : "OFF");
        return true;
    }
    if (code == "MOONRACE") {
        const bool on = Toggle(g_state.low_gravity);
        ApplyLowGravity();
        LOGI("[OK] cheats: MOONRACE -> Low Gravity %s", on ? "ON" : "OFF");
        return true;
    }
    if (code == "NOCOPS") {
        const bool on = Toggle(g_state.no_cops);
        if (on) {
            g_state.more_cops.store(false, std::memory_order_relaxed);
            g_state.more_more_cops.store(false, std::memory_order_relaxed);
        }
        LOGI("[OK] cheats: NOCOPS -> %s", on ? "ON" : "OFF");
        return true;
    }
    if (code == "MORECOPS") {
        const bool on = Toggle(g_state.more_cops);
        if (on) {
            g_state.no_cops.store(false, std::memory_order_relaxed);
            g_state.more_more_cops.store(false, std::memory_order_relaxed);
        }
        LOGI("[OK] cheats: MORECOPS -> %s", on ? "ON" : "OFF");
        return true;
    }
    if (code == "MOREMORECOPS") {
        const bool on = Toggle(g_state.more_more_cops);
        if (on) {
            g_state.no_cops.store(false, std::memory_order_relaxed);
            g_state.more_cops.store(false, std::memory_order_relaxed);
        }
        LOGI("[OK] cheats: MOREMORECOPS -> %s", on ? "ON" : "OFF");
        return true;
    }
    if (code == "ZEROCOST") {
        const bool on = Toggle(g_state.zero_cost);
        LOGI("[OK] cheats: ZEROCOST -> Shop Always Free %s", on ? "ON" : "OFF");
        return true;
    }
    if (code == "ARCADE") {
        const bool on = Toggle(g_state.arcade_powerups);
        if (on) ApplyArcadeSettings();
        LOGI("[OK] cheats: ARCADE -> Power Pickups %s", on ? "ON" : "OFF");
        return true;
    }
    if (code == "SUPERCOPY") {
        const bool on = Toggle(g_state.super_copy);
        if (on) ApplySuperCopy();
        LOGI("[OK] cheats: SUPERCOPY -> %s", on ? "ON" : "OFF");
        return true;
    }
    if (code == "DELIVERY") {
        const bool on = Toggle(g_state.delivery);
        if (on) ApplyUnlockFlags();
        LOGI("[OK] cheats: DELIVERY -> Unlock All Cities %s", on ? "ON" : "OFF");
        return true;
    }
    return false;
}

template <typename Fn>
void RunWithZeroCost(X86Context& ctx, uint8_t* base, Fn orig_fn) {
    if (!g_state.zero_cost.load(std::memory_order_relaxed)) {
        orig_fn(ctx, base);
        return;
    }
    const uint32_t prof = GetProfilePtr();
    uint32_t saved_cash = 0u;
    bool patched = false;
    if (prof && safe_mem::CanAccess(prof + kProfileCashOffset, 4u)) {
        saved_cash = kernel::R32(prof + kProfileCashOffset);
        kernel::W32(prof + kProfileCashOffset, 99999999u);
        patched = true;
    }
    orig_fn(ctx, base);
    if (patched && safe_mem::CanAccess(prof + kProfileCashOffset, 4u)) {
        kernel::W32(prof + kProfileCashOffset, saved_cash);
    }
}

}  // namespace

void OnFrameTick() {
    ApplyUnlockFlags();
    ApplyTrafficSettings();
    ApplyArcadeSettings();
    ApplyLowGravity();
    ApplyFullThrottleNos();
    ApplyPoliceCheats();
}

}  // namespace game::cheats

// Cheat code input dispatcher: __stdcall int sub_001923B0(const char* input) [ret 0x04].
// Note: sub_00192350 scrambles the plain-text input to compare against the obfuscated
// built-in table; input itself is already plain ASCII from the keyboard UI.
X86_FUNC(sub_001923B0) {
    const uint32_t input_va = X86_MEM32(ctx.esp + 4u);
    if (input_va) {
        const std::string input = game::safe_mem::ReadSanitizedCString(input_va, 64);
        if (!input.empty() && game::cheats::TryHandleCheatCode(input)) {
            ctx.eax = 0xFFFFFFFFu;
            ctx.esp += 8u;
            return;
        }
    }
    __imp__sub_001923B0(ctx, base);
}

// Unlock check: __thiscall bool sub_00292CD0(uint32_t type, uint32_t id, uint32_t def) [ret 0x0C].
X86_FUNC(sub_00292CD0) {
    const uint32_t type = X86_MEM32(ctx.esp + 4u);
    if (type == 5u && game::cheats::g_state.delivery.load(std::memory_order_relaxed)) {
        ctx.eax = (ctx.eax & 0xFFFFFF00u) | 1u;
        ctx.esp += 16u;
        return;
    }
    if ((type == 1u || type == 2u) &&
        game::cheats::g_state.secret_dealer.load(std::memory_order_relaxed)) {
        ctx.eax = (ctx.eax & 0xFFFFFF00u) | 1u;
        ctx.esp += 16u;
        return;
    }
    __imp__sub_00292CD0(ctx, base);
}

// Police pursuit state setter: __thiscall void sub_001CF9D0(uint32_t racer_idx, uint32_t state) [ret 0x08].
X86_FUNC(sub_001CF9D0) {
    const uint32_t racer_idx = X86_MEM32(ctx.esp + 4u);
    if (racer_idx == 0u && game::cheats::g_state.no_cops.load(std::memory_order_relaxed)) {
        X86_MEM32(ctx.esp + 8u) = 0u;
    }
    __imp__sub_001CF9D0(ctx, base);
}

// Race settings sync before race load.
X86_FUNC(sub_00269220) {
    __imp__sub_00269220(ctx, base);
    game::cheats::ApplyTrafficSettings();
    game::cheats::ApplyArcadeSettings();
    game::cheats::ApplySuperCopy();
}

// Race initialization (spawns HUD, powerup pickups, traffic, police, racers for both Career and Arcade).
X86_FUNC(sub_00029337) {
    game::cheats::ApplyTrafficSettings();
    game::cheats::ApplyArcadeSettings();
    game::cheats::ApplySuperCopy();
    game::cheats::ApplyPoliceCheats();
    __imp__sub_00029337(ctx, base);
    game::cheats::SpawnRandomArcadePowerupsIfEmpty(ctx, base);
}

// Race vehicle spawn/init.
X86_FUNC(sub_002BD990) {
    game::cheats::ApplySuperCopy();
    __imp__sub_002BD990(ctx, base);
}

// Shop / Garage purchase hooks for ZEROCOST.
X86_FUNC(sub_001422D0) { game::cheats::RunWithZeroCost(ctx, base, __imp__sub_001422D0); }
X86_FUNC(sub_00144C90) { game::cheats::RunWithZeroCost(ctx, base, __imp__sub_00144C90); }
X86_FUNC(sub_001474A0) { game::cheats::RunWithZeroCost(ctx, base, __imp__sub_001474A0); }
X86_FUNC(sub_0014E300) { game::cheats::RunWithZeroCost(ctx, base, __imp__sub_0014E300); }
