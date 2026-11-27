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

X86_EXTERN_FUNC(sub_00326FE0);
X86_EXTERN_FUNC(sub_00327A20);

X86_EXTERN_FUNC(__imp__sub_001923B0);
X86_EXTERN_FUNC(__imp__sub_00292CD0);
X86_EXTERN_FUNC(__imp__sub_00153510);
X86_EXTERN_FUNC(__imp__sub_00269220);
X86_EXTERN_FUNC(__imp__sub_00029337);
X86_EXTERN_FUNC(__imp__sub_00029690);
X86_EXTERN_FUNC(__imp__sub_00327830);
X86_EXTERN_FUNC(__imp__sub_001D4F20);
X86_EXTERN_FUNC(__imp__sub_001015E0);
X86_EXTERN_FUNC(__imp__sub_00329060);
X86_EXTERN_FUNC(__imp__sub_0010B320);
X86_EXTERN_FUNC(__imp__sub_0010B370);
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
constexpr uint32_t kUnlockAllCitiesFlag1 = 0x0053F2ECu;
constexpr uint32_t kUnlockAllCitiesFlag2 = 0x0053F2EDu;
constexpr uint32_t kTrafficGlobal1 = 0x004B36C8u;
constexpr uint32_t kTrafficGlobal2 = 0x004B36CCu;
constexpr uint32_t kVehicleListPtrAddr = 0x00557EB0u;
constexpr uint32_t kVehicleListCountAddr = 0x00557EB4u;
constexpr uint32_t kGameStateAddr = 0x0053F2D4u;

struct CheatState {
    std::atomic<bool> full_throttle{false};
    std::atomic<bool> secret_dealer{false};
    std::atomic<bool> no_traffic{false};
    std::atomic<bool> zero_cost{false};
    std::atomic<bool> arcade_powerups{false};
    std::atomic<bool> delivery{false};

    bool powerup_hud_loaded_by_cheat = false;
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

// Vehicle list entries at [0x00557EB0] (stride 0x28) store bool SpecialEdition at +0x14
// (set by sub_0026A390 when "SpecialEdition" is present on REMIX/special prize vehicles).
bool IsSpecialEditionVehicle(uint32_t veh_id) {
    const uint16_t count = safe_mem::Read16(kVehicleListCountAddr, 0u);
    const uint32_t arr = safe_mem::Read32(kVehicleListPtrAddr, 0u);
    if (!arr || veh_id >= count) return false;
    const uint32_t entry = arr + veh_id * 0x28u;
    return safe_mem::Read8(entry + 0x14u, 0u) != 0u;
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

bool IsValidPowerupType(uint32_t type) {
    if (type >= 15u) return false;
    const uint32_t mgr = safe_mem::Read32(0x00533E38u, 0u);
    if (!mgr) return false;
    const uint32_t arr = safe_mem::Read32(mgr + 0x0Cu, 0u);
    if (!arr) return false;
    return safe_mem::Read32(arr + type * 4u, 0u) != 0u;
}

uint32_t PickValidPowerupType(uint32_t seed) {
    const uint32_t mgr = safe_mem::Read32(0x00533E38u, 0u);
    const uint32_t arr = mgr ? safe_mem::Read32(mgr + 0x0Cu, 0u) : 0u;
    if (!arr) return 0u;
    for (uint32_t off = 0; off < 13u; ++off) {
        const uint32_t cand = (seed + off) % 13u;
        if (safe_mem::Read32(arr + cand * 4u, 0u) != 0u) return cand;
    }
    return 0u;
}

void SpawnRandomArcadePowerupsForRaceMgr(X86Context& ctx, uint8_t* base, uint32_t race_mgr) {
    if (!g_state.arcade_powerups.load(std::memory_order_relaxed)) return;
    if (!race_mgr || !safe_mem::CanAccess(race_mgr + 0x24u, 8u) ||
        !safe_mem::CanAccess(race_mgr + 0x80u, 0x10u)) {
        return;
    }
    const uint16_t cur_count = safe_mem::Read16(race_mgr + 0x80u + 0x08u, 0u);
    if (cur_count > 0u) return;

    const int32_t num_checkpoints = static_cast<int32_t>(safe_mem::Read32(race_mgr + 0x24u, 0u));
    const uint32_t checkpoints_ptr = safe_mem::Read32(race_mgr + 0x28u, 0u);
    if (num_checkpoints <= 0 || num_checkpoints > 128 || !checkpoints_ptr) return;
    if (!safe_mem::CanAccess(checkpoints_ptr, static_cast<uint32_t>(num_checkpoints) * 0x120u)) return;

    // Select 1 checkpoint every 4-5 checkpoints (up to 16 selected checkpoints) and clamp width
    // to 15.0f so sub_00329850 spawns 3 pickups per selected checkpoint: ((15 - 3 - 3) / 3 = 3).
    int32_t selected[16];
    int32_t sel_count = 0;
    if (num_checkpoints <= 3) {
        selected[sel_count++] = num_checkpoints / 2;
    } else {
        int32_t idx = 2;
        bool step_five = false;
        while (idx < num_checkpoints && sel_count < 16) {
            selected[sel_count++] = idx;
            idx += step_five ? 5 : 4;
            step_five = !step_five;
        }
    }
    if (sel_count <= 0) return;

    const uint32_t orig_esp = ctx.esp;
    const uint32_t orig_eax = ctx.eax;
    const uint32_t orig_ecx = ctx.ecx;
    const uint32_t orig_edx = ctx.edx;

    const uint32_t tmp_bytes = static_cast<uint32_t>(sel_count) * 0x120u;
    ctx.esp = (ctx.esp - tmp_bytes) & ~0xFu;
    const uint32_t tmp_cp_va = ctx.esp;
    const float target_width = 15.0f;
    uint32_t target_width_bits = 0u;
    std::memcpy(&target_width_bits, &target_width, sizeof(target_width_bits));

    for (int32_t i = 0; i < sel_count; ++i) {
        const uint32_t src_va = checkpoints_ptr + static_cast<uint32_t>(selected[i]) * 0x120u;
        const uint32_t dst_va = tmp_cp_va + static_cast<uint32_t>(i) * 0x120u;
        std::memcpy(kernel::HostPtr(dst_va), kernel::HostPtr(src_va), 0x120u);
        kernel::W32(dst_va + 0x110u, target_width_bits);
    }

    ctx.ecx = race_mgr + 0x80u;
    X86_PUSH32(0u);
    X86_PUSH32(tmp_cp_va);
    X86_PUSH32(static_cast<uint32_t>(sel_count));
    X86_PUSH32(0x001D5999u);
    sub_00326FE0(ctx, base);

    ctx.esp = orig_esp;
    ctx.eax = orig_eax;
    ctx.ecx = orig_ecx;
    ctx.edx = orig_edx;

    // Sanitize all spawned pickup types so none reference NULL slots (13/14) in mcPowerUpMgr.
    const uint32_t pickups_ptr = safe_mem::Read32(race_mgr + 0x80u + 0x04u, 0u);
    const uint16_t spawned = safe_mem::Read16(race_mgr + 0x80u + 0x08u, 0u);
    if (pickups_ptr && spawned > 0u) {
        for (uint16_t i = 0; i < spawned; ++i) {
            const uint32_t p = pickups_ptr + static_cast<uint32_t>(i) * 0x4Cu;
            if (!safe_mem::CanAccess(p + 0x10u, 4u)) break;
            const uint32_t ptype = kernel::R32(p + 0x10u);
            if (!IsValidPowerupType(ptype)) {
                kernel::W32(p + 0x10u, PickValidPowerupType(i + ptype));
            }
        }
    }
    LOGI("[OK] cheats: ARCADE spawned %u randomized powerups across %d/%d checkpoints (~3 per 4-5 cp)",
         unsigned(spawned), sel_count, num_checkpoints);
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
        LOGI("[OK] cheats: SECRETDEALER -> REMIX/SpecialEdition cars %s", on ? "ON" : "OFF");
        return true;
    }
    if (code == "GHOSTTOWN" || code == "GENOSIDA") {
        const bool on = Toggle(g_state.no_traffic);
        if (on) ApplyTrafficSettings();
        LOGI("[OK] cheats: %s -> No Traffic %s", code.c_str(), on ? "ON" : "OFF");
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
    ApplyFullThrottleNos();
}

}  // namespace game::cheats

// Cheat code input dispatcher: __stdcall int sub_001923B0(const char* input) [ret 0x04].
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
    const uint32_t id = X86_MEM32(ctx.esp + 8u);
    if (type == 5u && game::cheats::g_state.delivery.load(std::memory_order_relaxed)) {
        ctx.eax = (ctx.eax & 0xFFFFFF00u) | 1u;
        ctx.esp += 16u;
        return;
    }
    // SECRETDEALER: only unlock locked REMIX (SpecialEdition) cars, never regular locked cars.
    if ((type == 1u || type == 2u) &&
        game::cheats::g_state.secret_dealer.load(std::memory_order_relaxed) &&
        game::cheats::IsSpecialEditionVehicle(id)) {
        ctx.eax = (ctx.eax & 0xFFFFFF00u) | 1u;
        ctx.esp += 16u;
        return;
    }
    __imp__sub_00292CD0(ctx, base);
}

// Showroom vehicle filter: __thiscall bool sub_00153510(car_entry, filter, allow_special_edition) [ret 0x0C].
// In stock Showroom, allow_special_edition == 0 hides SpecialEdition (REMIX) cars so they can't be bought.
X86_FUNC(sub_00153510) {
    if (game::cheats::g_state.secret_dealer.load(std::memory_order_relaxed)) {
        X86_MEM32(ctx.esp + 0x0Cu) = 1u;
    }
    __imp__sub_00153510(ctx, base);
}

// Race settings sync before race load.
X86_FUNC(sub_00269220) {
    __imp__sub_00269220(ctx, base);
    game::cheats::ApplyTrafficSettings();
    game::cheats::ApplyArcadeSettings();
}

// Race initialization (spawns HUD, powerup pickups, traffic for both Career and Arcade).
X86_FUNC(sub_00029337) {
    game::cheats::ApplyTrafficSettings();
    if (game::cheats::g_state.arcade_powerups.load(std::memory_order_relaxed)) {
        game::cheats::ApplyArcadeSettings();
        game::cheats::g_state.powerup_hud_loaded_by_cheat = true;
    }
    __imp__sub_00029337(ctx, base);
}

// Guard sub_00327830 against double-loading powerup assets into the fixed Xbox heap.
X86_FUNC(sub_00327830) {
    if (game::safe_mem::Read8(0x00563730u, 0u) != 0u) {
        ctx.esp += 4u;
        return;
    }
    __imp__sub_00327830(ctx, base);
}

// Race unload: stock sub_00029690 only calls sub_00327A20 (unload powerup models/textures)
// when [0x0053F2D4] == 1 (Arcade). Unload in Career too so the fixed Xbox heap doesn't overrun.
X86_FUNC(sub_00029690) {
    if (game::cheats::g_state.powerup_hud_loaded_by_cheat) {
        const uint32_t rs = game::safe_mem::Read32(game::cheats::kRaceSettingsPtrAddr, 0u);
        if (rs && game::safe_mem::CanAccess(rs + 0x1Cu, 4u)) {
            kernel::W32(rs + 0x1Cu, 1u);
        }
        game::cheats::g_state.powerup_hud_loaded_by_cheat = false;
    }
    __imp__sub_00029690(ctx, base);
    if (game::safe_mem::Read8(0x00563730u, 0u) != 0u) {
        const uint32_t orig_esp = ctx.esp;
        const uint32_t orig_eax = ctx.eax;
        const uint32_t orig_ecx = ctx.ecx;
        const uint32_t orig_edx = ctx.edx;
        X86_PUSH32(0x000296CFu);
        sub_00327A20(ctx, base);
        ctx.esp = orig_esp;
        ctx.eax = orig_eax;
        ctx.ecx = orig_ecx;
        ctx.edx = orig_edx;
        LOGI("[OK] cheats: unloaded Career powerup assets via sub_00327A20");
    }
}

// Race file loader: __thiscall bool sub_001D4F20(a1, a2) [ret 0x08].
// After checkpoints are loaded, spawn Paint-style randomized powerup pickups if ARCADE is active.
X86_FUNC(sub_001D4F20) {
    const uint32_t race_mgr = ctx.ecx;
    __imp__sub_001D4F20(ctx, base);
    if ((ctx.eax & 0xFFu) != 0u) {
        game::cheats::SpawnRandomArcadePowerupsForRaceMgr(ctx, base, race_mgr);
    }
}

// Guard powerup pickup handler against NULL component, NULL vehicle, or NULL powerup slot (13/14).
// Signature: __thiscall bool sub_001015E0(uint32_t type, uint32_t flag, uint32_t pos_ptr) [ret 0x0C].
X86_FUNC(sub_001015E0) {
    const uint32_t comp = ctx.ecx;
    const uint32_t ptype = X86_MEM32(ctx.esp + 4u);
    if (!comp || !game::safe_mem::CanAccess(comp + 0x04u, 0x80u) ||
        game::safe_mem::Read32(comp + 0x04u, 0u) == 0u) {
        ctx.eax &= 0xFFFFFF00u;
        ctx.esp += 16u;
        return;
    }
    if (ptype != 0u && !game::cheats::IsValidPowerupType(ptype)) {
        ctx.eax &= 0xFFFFFF00u;
        ctx.esp += 16u;
        return;
    }
    __imp__sub_001015E0(ctx, base);
}

// Sanitize respawned random powerup types so pickups never roll NULL slots (13/14).
// Signature: __thiscall void sub_00329060(uint32_t rand_seed) [ret 0x04].
X86_FUNC(sub_00329060) {
    const uint32_t pickup = ctx.ecx;
    const uint32_t seed = X86_MEM32(ctx.esp + 4u);
    __imp__sub_00329060(ctx, base);
    if (pickup && game::safe_mem::CanAccess(pickup + 0x10u, 4u)) {
        const uint32_t chosen = kernel::R32(pickup + 0x10u);
        if (chosen < 16u && !game::cheats::IsValidPowerupType(chosen)) {
            kernel::W32(pickup + 0x10u, game::cheats::PickValidPowerupType(seed + chosen));
        }
    }
}

// Guard powerup HUD display notifications against NULL HUD pointers in Career races.
// Both sub_0010B320 and sub_0010B370 are __thiscall [ret 0x04].
X86_FUNC(sub_0010B320) {
    if (!ctx.ecx || !game::safe_mem::CanAccess(ctx.ecx + 0x48u, 8u)) {
        ctx.esp += 8u;
        return;
    }
    __imp__sub_0010B320(ctx, base);
}

X86_FUNC(sub_0010B370) {
    if (!ctx.ecx || !game::safe_mem::CanAccess(ctx.ecx + 0x48u, 8u)) {
        ctx.esp += 8u;
        return;
    }
    __imp__sub_0010B370(ctx, base);
}

// Shop / Garage purchase hooks for ZEROCOST.
X86_FUNC(sub_001422D0) { game::cheats::RunWithZeroCost(ctx, base, __imp__sub_001422D0); }
X86_FUNC(sub_00144C90) { game::cheats::RunWithZeroCost(ctx, base, __imp__sub_00144C90); }
X86_FUNC(sub_001474A0) { game::cheats::RunWithZeroCost(ctx, base, __imp__sub_001474A0); }
X86_FUNC(sub_0014E300) { game::cheats::RunWithZeroCost(ctx, base, __imp__sub_0014E300); }
