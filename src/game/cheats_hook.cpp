#include "game/cheats_hook.h"
#include "game/touch_context.h"

#include "app/app.h"
#include "common/mouse_keyboard.h"
#include "core/log.h"
#include "cpu/x86_ops.h"
#include "game/guest_mem_safe.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

X86_EXTERN_FUNC(sub_00326FE0);
X86_EXTERN_FUNC(sub_00327A20);

X86_EXTERN_FUNC(__imp__sub_000197C0);
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
X86_EXTERN_FUNC(__imp__sub_001442A0);
X86_EXTERN_FUNC(__imp__sub_00144C90);
X86_EXTERN_FUNC(__imp__sub_001474A0);
X86_EXTERN_FUNC(__imp__sub_0014E300);
X86_EXTERN_FUNC(__imp__sub_000B2F80);
X86_EXTERN_FUNC(__imp__sub_00139520);
X86_EXTERN_FUNC(__imp__sub_001395E0);

namespace game::cheats {
namespace {

constexpr uint32_t kSaveMgrGlobalPtrAddr = 0x004CB690u;
constexpr uint32_t kSaveMgrNameOffset = 0x334B4u;
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

struct CachedTxtPrompt {
    uint32_t clip = 0;
    uint32_t name_va = 0;
    uint32_t name_len = 0;
    uint32_t str_va = 0;
};

constexpr size_t kMaxCachedTxtPrompts = 16;
CachedTxtPrompt g_txt_prompts[kMaxCachedTxtPrompts]{};
uint32_t g_prompt_clip_va = 0;
uint32_t g_want_show_prompt = 0;
uint32_t g_want_show_prompt_help = 0;
bool g_has_show_prompt = false;
bool g_has_show_prompt_help = false;
bool g_last_prompt_touch = true;

void CallFlashIntVar(X86Context& ctx, uint8_t* base, uint32_t clip, uint32_t name_va, uint32_t val) {
    const uint32_t saved_eax = ctx.eax, saved_ecx = ctx.ecx, saved_edx = ctx.edx, saved_esp = ctx.esp;
    ctx.ecx = clip;
    X86_PUSH32(val);
    X86_PUSH32(name_va);
    X86_PUSH32(0u);
    __imp__sub_00139520(ctx, base);
    ctx.eax = saved_eax;
    ctx.ecx = saved_ecx;
    ctx.edx = saved_edx;
    ctx.esp = saved_esp;
}

void CallFlashStringVar(X86Context& ctx, uint8_t* base, uint32_t clip, uint32_t name_va, uint32_t str_va) {
    const uint32_t saved_eax = ctx.eax, saved_ecx = ctx.ecx, saved_edx = ctx.edx, saved_esp = ctx.esp;
    ctx.ecx = clip;
    X86_PUSH32(str_va);
    X86_PUSH32(name_va);
    X86_PUSH32(0u);
    __imp__sub_001395E0(ctx, base);
    ctx.eax = saved_eax;
    ctx.ecx = saved_ecx;
    ctx.edx = saved_edx;
    ctx.esp = saved_esp;
}

void RecordTxtPrompt(uint32_t clip, uint32_t name_va, uint32_t name_len, uint32_t str_va) {
    size_t free_idx = kMaxCachedTxtPrompts;
    for (size_t i = 0; i < kMaxCachedTxtPrompts; ++i) {
        if (g_txt_prompts[i].clip == clip && g_txt_prompts[i].name_va == name_va) {
            g_txt_prompts[i].name_len = name_len;
            g_txt_prompts[i].str_va = str_va;
            return;
        }
        if (free_idx == kMaxCachedTxtPrompts && g_txt_prompts[i].clip == 0u) free_idx = i;
    }
    const size_t slot = (free_idx < kMaxCachedTxtPrompts) ? free_idx : 0u;
    g_txt_prompts[slot] = {clip, name_va, name_len, str_va};
}

}  // namespace

std::atomic<uint32_t> g_full_map_obj{0};
std::atomic<bool> g_full_map_open{false};

void RecordPromptInt(uint32_t clip, bool is_prompt, uint32_t val) {
    g_prompt_clip_va = clip;
    if (is_prompt) {
        g_want_show_prompt = val;
        g_has_show_prompt = true;
    } else {
        g_want_show_prompt_help = val;
        g_has_show_prompt_help = true;
    }
}

void RecordPromptString(uint32_t clip, uint32_t name_va, uint32_t name_len, uint32_t str_va) {
    RecordTxtPrompt(clip, name_va, name_len, str_va);
}

void SyncPromptVisibility(X86Context& ctx, uint8_t* base) {
    const bool touch_now = input::TouchActive();
    if (touch_now == g_last_prompt_touch) return;
    g_last_prompt_touch = touch_now;

    const uint32_t flash_root = safe_mem::Read32(0x0050F15Cu, 0u);
    const uint32_t live_clip = flash_root ? safe_mem::Read32(flash_root + 0xFACu, 0u) : 0u;
    if (live_clip != 0u && live_clip == g_prompt_clip_va && safe_mem::CanAccess(live_clip, 16u)) {
        if (g_has_show_prompt) {
            CallFlashIntVar(ctx, base, live_clip, 0x00457C10u, touch_now ? 0u : g_want_show_prompt);
        }
        if (g_has_show_prompt_help) {
            CallFlashIntVar(ctx, base, live_clip, 0x00457C30u, touch_now ? 0u : g_want_show_prompt_help);
        }
    }

    uint32_t nav_ui = 0u;
    if (g_full_map_open.load(std::memory_order_relaxed)) {
        const uint32_t fm = g_full_map_obj.load(std::memory_order_relaxed);
        nav_ui = fm ? safe_mem::Read32(fm + 0x1F0u, 0u) : 0u;
    }
    for (const CachedTxtPrompt& e : g_txt_prompts) {
        if (e.clip == 0u || e.name_va == 0u) continue;
        if (e.clip != live_clip && (nav_ui == 0u || e.clip != nav_ui)) continue;
        if (!safe_mem::CanAccess(e.clip, 16u)) continue;
        const uint32_t target_str = touch_now ? (e.name_va + e.name_len) : e.str_va;
        if (target_str != 0u && safe_mem::CanAccess(target_str, 1u)) {
            CallFlashStringVar(ctx, base, e.clip, e.name_va, target_str);
        }
    }
}

namespace {
constexpr uint32_t kGameStateAddr = 0x0053F2D4u;

enum class CheatId : int {
    Cash5000 = 0,
    FullThrottle = 1,
    SecretDealer = 2,
    NoTraffic = 3,
    ZeroCost = 4,
    Arcade = 5,
    Delivery = 6,
    Count = 7,
};

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
thread_local int g_zero_cost_depth = 0;

std::mutex g_save_mutex;
std::string g_synced_save_name;
std::atomic<bool> g_legacy_db_cleaned{false};

const char* GetCheatCodeName(CheatId id) {
    switch (id) {
        case CheatId::Cash5000:     return "AMSSRBLHGSHARK";
        case CheatId::FullThrottle: return "FULLTHROTTLE";
        case CheatId::SecretDealer: return "SECRETDEALER";
        case CheatId::NoTraffic:    return "GHOSTTOWN";
        case CheatId::ZeroCost:     return "ZEROCOST";
        case CheatId::Arcade:       return "ARCADE";
        case CheatId::Delivery:     return "DELIVERY";
        default:                    return "UNKNOWN";
    }
}

std::string UpperTrim(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (unsigned char c : raw) {
        if (!std::isspace(c)) out.push_back(static_cast<char>(std::toupper(c)));
    }
    return out;
}

std::string TrimString(const std::string& s) {
    size_t a = 0;
    while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    size_t b = s.size();
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

bool ParseCheatName(const std::string& raw, CheatId* out_id) {
    const std::string code = UpperTrim(raw);
    if (code == "AMSSRBLHGSHARK") { *out_id = CheatId::Cash5000; return true; }
    if (code == "FULLTHROTTLE") { *out_id = CheatId::FullThrottle; return true; }
    if (code == "SECRETDEALER") { *out_id = CheatId::SecretDealer; return true; }
    if (code == "GHOSTTOWN" || code == "GENOSIDA") { *out_id = CheatId::NoTraffic; return true; }
    if (code == "ZEROCOST") { *out_id = CheatId::ZeroCost; return true; }
    if (code == "ARCADE") { *out_id = CheatId::Arcade; return true; }
    if (code == "DELIVERY") { *out_id = CheatId::Delivery; return true; }
    return false;
}

void DeleteLegacyCheatDbOnce() {
    if (g_legacy_db_cleaned.exchange(true, std::memory_order_relaxed)) return;
    std::error_code ec;
    const auto db_path = app::ExeDir() / "saves_cheat.db";
    if (std::filesystem::remove(db_path, ec)) {
        LOGI("[OK] cheats: removed legacy dedicated saved cheat file saves_cheat.db");
    }
    ec.clear();
    auto tmp_path = db_path;
    tmp_path += ".tmp";
    std::filesystem::remove(tmp_path, ec);
}

void ClearAllSessionCheats() {
    g_state.full_throttle.store(false, std::memory_order_relaxed);
    g_state.secret_dealer.store(false, std::memory_order_relaxed);
    g_state.no_traffic.store(false, std::memory_order_relaxed);
    g_state.zero_cost.store(false, std::memory_order_relaxed);
    g_state.arcade_powerups.store(false, std::memory_order_relaxed);
    g_state.delivery.store(false, std::memory_order_relaxed);
}

std::string ReadGuestActiveSaveName() {
    const uint32_t mgr = safe_mem::Read32(kSaveMgrGlobalPtrAddr, 0u);
    if (!mgr) return {};
    if (!safe_mem::CanAccess(mgr + kSaveMgrNameOffset, 4u)) return {};
    return TrimString(safe_mem::ReadSanitizedCString(mgr + kSaveMgrNameOffset, 64));
}

// Session-only cheats: never persisted across restarts, and automatically cleared when switching/unloading saves.
void ResetCheatsOnSaveSwitch() {
    DeleteLegacyCheatDbOnce();
    const std::string active_save = ReadGuestActiveSaveName();
    std::lock_guard<std::mutex> lock(g_save_mutex);
    if (active_save == g_synced_save_name) return;

    const std::string prev_save = g_synced_save_name;
    g_synced_save_name = active_save;
    if (active_save.empty() || !prev_save.empty()) {
        ClearAllSessionCheats();
        LOGI("[OK] cheats: save changed ('%s' -> '%s'), all session cheats reset OFF",
             prev_save.c_str(), active_save.c_str());
    }
}

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

std::atomic<bool>* FlagForCheat(CheatId id) {
    switch (id) {
        case CheatId::FullThrottle: return &g_state.full_throttle;
        case CheatId::SecretDealer: return &g_state.secret_dealer;
        case CheatId::NoTraffic:    return &g_state.no_traffic;
        case CheatId::ZeroCost:     return &g_state.zero_cost;
        case CheatId::Arcade:       return &g_state.arcade_powerups;
        case CheatId::Delivery:     return &g_state.delivery;
        default:                    return nullptr;
    }
}

bool TryHandleCheatCode(const std::string& raw) {
    ResetCheatsOnSaveSwitch();
    CheatId id;
    if (!ParseCheatName(raw, &id)) return false;

    if (id == CheatId::Cash5000) {
        const bool ok = AddCash(5000);
        LOGI("[OK] cheats: AMSSRBLHGSHARK -> +5000 cash (%s)", ok ? "applied" : "no-profile");
        return true;
    }

    auto* flag = FlagForCheat(id);
    if (!flag) return false;
    const bool on = !flag->load(std::memory_order_relaxed);
    flag->store(on, std::memory_order_relaxed);
    if (on) {
        if (id == CheatId::NoTraffic) ApplyTrafficSettings();
        else if (id == CheatId::Arcade) ApplyArcadeSettings();
        else if (id == CheatId::Delivery) ApplyUnlockFlags();
    }
    LOGI("[OK] cheats: %s -> %s (session-only)", GetCheatCodeName(id), on ? "ON" : "OFF");
    return true;
}

template <typename Fn>
void RunWithZeroCost(X86Context& ctx, uint8_t* base, Fn orig_fn) {
    if (!g_state.zero_cost.load(std::memory_order_relaxed)) {
        orig_fn(ctx, base);
        return;
    }
    const bool outer = (g_zero_cost_depth == 0);
    ++g_zero_cost_depth;
    const uint32_t prof = outer ? GetProfilePtr() : 0u;
    uint32_t saved_cash = 0u;
    bool patched = false;
    if (outer && prof && safe_mem::CanAccess(prof + kProfileCashOffset, 4u)) {
        saved_cash = kernel::R32(prof + kProfileCashOffset);
        kernel::W32(prof + kProfileCashOffset, 99999999u);
        patched = true;
    }
    orig_fn(ctx, base);
    if (patched && safe_mem::CanAccess(prof + kProfileCashOffset, 4u)) {
        kernel::W32(prof + kProfileCashOffset, saved_cash);
    }
    --g_zero_cost_depth;
}

}  // namespace

uint64_t SteadyNowMs() {
    auto since = std::chrono::steady_clock::now().time_since_epoch();
    return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(since).count()) + 1u;
}

bool IsUnskippableMovieName(const std::string& raw_name) {
    if (raw_name.empty()) return false;
    const size_t slash = raw_name.find_last_of("\\/");
    std::string name = (slash == std::string::npos) ? raw_name : raw_name.substr(slash + 1u);
    for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return name.find("rockstar") != std::string::npos ||
           name.find("sdlogo") != std::string::npos ||
           name.find("sandiego") != std::string::npos ||
           name.find("legal") != std::string::npos ||
           name.find("esrb") != std::string::npos;
}

void UpdateTouchTelemetry() {
    auto& tel = game::Telemetry();

    const uint64_t now_ms = SteadyNowMs();
    const uint64_t last_fmv = tel.fmv_frame_ms.load(std::memory_order_relaxed);
    const bool fmv_recent = (last_fmv != 0u) && (now_ms >= last_fmv) && ((now_ms - last_fmv) < 220u);

    const uint32_t state_mgr = safe_mem::Read32(0x00557ED0u, 0u);
    const uint32_t app_state = state_mgr ? safe_mem::Read32(state_mgr + 0x04u, 0u) : 0u;

    // Live movie name directly from MC3D's MovieManager ([0x0053F2E4] + 0xE8) used by sub_0015ACB0
    // ("rockstar", "sdlogo", "mc3intro", story cutscenes, etc.).
    bool fmv_unskippable = tel.fmv_unskippable.load(std::memory_order_relaxed);
    if (app_state == 0x04u || fmv_recent) {
        const uint32_t movie_mgr = safe_mem::Read32(0x0053F2E4u, 0u);
        if (movie_mgr != 0u && safe_mem::CanAccess(movie_mgr + 0xE8u, 4u)) {
            const std::string live_movie = safe_mem::ReadSanitizedCString(movie_mgr + 0xE8u, 64u);
            if (!live_movie.empty()) {
                fmv_unskippable = IsUnskippableMovieName(live_movie);
                tel.fmv_unskippable.store(fmv_unskippable, std::memory_order_relaxed);
            }
        }
    }

    const uint32_t menu_mgr = safe_mem::Read32(0x0050F148u, 0u);
    const uint32_t menu_obj = menu_mgr ? safe_mem::Read32(menu_mgr + 0x0Cu, 0u) : 0u;
    const uint32_t menu_owner = menu_obj ? safe_mem::Read32(menu_obj + 0x0Cu, 0u) : 0u;
    const bool menu_visible = menu_owner && ((safe_mem::Read8(menu_owner + 0x44u, 0u) & 0x01u) != 0u);

    // Exact MC3D in-game Pause flags verified from sub_00154550 (PauseGame) & sub_001545C0 (ResumeGame):
    //   [state_mgr + 0x09] = sim paused, [0x0050F159] = global UI pause, [menu_mgr + 0x04] = mcUiManager paused
    const bool is_paused =
        (state_mgr != 0u && safe_mem::Read8(state_mgr + 0x09u, 0u) != 0u) ||
        (safe_mem::Read8(0x0050F159u, 0u) != 0u) ||
        (menu_mgr != 0u && safe_mem::Read8(menu_mgr + 0x04u, 0u) != 0u) ||
        menu_visible;

    const uint32_t car_mgr = safe_mem::Read32(0x00558814u, 0u);
    const uint32_t player_car = car_mgr ? safe_mem::Read32(car_mgr + 0x08u, 0u) : 0u;

    // Live Full City Map state (NavSys sub_000B2F80 + [ [nav_mgr + 0x1F0] + 0x28 ] is_open flag)
    bool in_city_map = false;
    if ((app_state == 0x02u || app_state == 0x03u) &&
        game::cheats::g_full_map_open.load(std::memory_order_relaxed)) {
        const uint32_t fm = game::cheats::g_full_map_obj.load(std::memory_order_relaxed);
        const uint32_t nav_ui = fm ? safe_mem::Read32(fm + 0x1F0u, 0u) : 0u;
        if (nav_ui != 0u && safe_mem::Read8(nav_ui + 0x28u, 0u) != 0u) {
            in_city_map = true;
        } else {
            game::cheats::g_full_map_open.store(false, std::memory_order_relaxed);
        }
    } else if (app_state != 0x02u && app_state != 0x03u) {
        game::cheats::g_full_map_open.store(false, std::memory_order_relaxed);
    }

    // Verified from default.xbe jump table @ 0x0001FEDC:
    //   0x01 = Boot/Idle, 0x02/0x03 = In-Game Driving/Pause, 0x04 = FMV/Movie,
    //   0x05 = Garage/Showroom (0x0050F12C), 0x06/0x07 = Frontend / Main Menu / Title / Options
    UiContext ctx = UiContext::Loading;
    if (app_state == 0x04u || (fmv_recent && app_state <= 0x01u)) {
        ctx = fmv_unskippable ? UiContext::FmvUnskippable : UiContext::FmvSkippable;
    } else if (app_state == 0x05u) {
        ctx = UiContext::Garage;
    } else if (app_state == 0x06u || app_state == 0x07u) {
        ctx = UiContext::MainMenu;
    } else if (app_state == 0x02u || app_state == 0x03u) {
        if (is_paused || in_city_map) ctx = UiContext::MainMenu;
        else if (player_car != 0u) ctx = UiContext::Gameplay;
        else ctx = UiContext::Loading;
    } else {
        ctx = UiContext::Loading;
    }
    tel.ui_context.store(static_cast<uint8_t>(ctx), std::memory_order_relaxed);

    // Load/Save Profile screen detection (bg_savegame.swf: flash_bg_id == 4 or screen_id 0x28 / 0x29)
    const uint32_t screen_id = menu_owner ? safe_mem::Read32(menu_owner + 0xD8u, 0u) : 0u;
    const uint32_t flash_root = safe_mem::Read32(0x0050F15Cu, 0u);
    const uint32_t flash_bg_id = flash_root ? safe_mem::Read32(flash_root + 0xFA8u, 0u) : 0u;
    const bool in_save_screen = (ctx == UiContext::MainMenu) && !in_city_map &&
                                (screen_id == 0x28u || screen_id == 0x29u || flash_bg_id == 4u);
    tel.in_save_screen.store(in_save_screen, std::memory_order_relaxed);
    tel.in_city_map.store(in_city_map, std::memory_order_relaxed);

    bool has_nos = false;
    bool turbo_ready = false;
    bool special_ready = false;
    bool special_full = false;
    int8_t special_type = -1;
    float speed_kmh = 0.0f;
    bool is_bike = false;
    bool race_countdown = false;

    if (ctx == UiContext::Gameplay && player_car != 0u) {
        const uint32_t veh_p14 = safe_mem::Read32(player_car + 0x14u, 0u);
        const uint32_t veh_phys = veh_p14 ? safe_mem::Read32(veh_p14 + 0x08u, 0u) : 0u;
        if (veh_phys && safe_mem::CanAccess(veh_phys + 0xB0u, 4u)) {
            const uint32_t speed_bits = kernel::R32(veh_phys + 0xB0u);
            float speed_mps = 0.0f;
            std::memcpy(&speed_mps, &speed_bits, sizeof(speed_mps));
            if (std::isfinite(speed_mps)) {
                speed_kmh = std::fabs(speed_mps) * 3.6f;
            }
        }

        // Exact MC3D IsMotorcycle() check (sub_000FF200): [[[player_car + 0x14] + 0x4C] + 0x08] + 0x3808 == 2 wheels.
        // Works for 100% of Choppers and Sport Bikes across all game modes.
        const uint32_t veh_p4c = veh_p14 ? safe_mem::Read32(veh_p14 + 0x4Cu, 0u) : 0u;
        const uint32_t veh_p08 = veh_p4c ? safe_mem::Read32(veh_p4c + 0x08u, 0u) : 0u;
        is_bike = (veh_p08 != 0u) && (safe_mem::Read32(veh_p08 + 0x3808u, 0u) == 2u);

        // Exact MC3D 3-2-1 race start countdown detection verified from mcRaceTimer::UpdateCountdown (sub_001C20E0):
        // race_timer = [0x00557EEC]; during a real pre-race 3-2-1 staging countdown:
        //   - [race_timer + 0x04] == 4 (countdown state; flips to 5 on the exact GO! frame)
        //   - [race_timer + 0x24] != 0 (show_countdown flag from !DontShowCountdown)
        //   - float [race_timer + 0x0C] is strictly negative (-3.0s .. 0.0s), NEVER 0.0f (idle/Cruise)!
        const uint32_t race_timer = safe_mem::Read32(0x00557EECu, 0u);
        if (race_timer != 0u && safe_mem::Read32(race_timer + 0x04u, 0u) == 0x04u &&
            safe_mem::Read8(race_timer + 0x24u, 0u) != 0u) {
            float t_sec = 0.0f;
            if (safe_mem::CanAccess(race_timer + 0x0Cu, 4u)) {
                const uint32_t tbits = kernel::R32(race_timer + 0x0Cu);
                std::memcpy(&t_sec, &tbits, sizeof(t_sec));
            }
            race_countdown = std::isfinite(t_sec) && (t_sec < -0.02f) && (t_sec >= -10.0f) && (speed_kmh < 8.0f);
        }

        // NOS: only show when player has purchased nitrous on this vehicle (max_nos > 0).
        const uint32_t nitro = veh_phys ? safe_mem::Read32(veh_phys + 0x180u, 0u) : 0u;
        if (nitro && safe_mem::CanAccess(nitro + 0x0Eu, 2u)) {
            const int16_t max_nos = static_cast<int16_t>(kernel::R16(nitro + 0x0Eu));
            has_nos = (max_nos > 0);
        }

        // Slipstream Turbo: only show when filled from slipstream (ready && speed >= 10 km/h, never during burnout).
        const uint32_t turbo = veh_phys ? safe_mem::Read32(veh_phys + 0x184u, 0u) : 0u;
        if (turbo && safe_mem::CanAccess(turbo + 0x24u, 8u)) {
            const uint32_t fill_bits = kernel::R32(turbo + 0x24u);
            float fill = 0.0f;
            std::memcpy(&fill, &fill_bits, sizeof(fill));
            const uint8_t firing = safe_mem::Read8(turbo + 0x28u, 0u);
            const uint8_t avail = safe_mem::Read8(turbo + 0x29u, 0u);
            turbo_ready = (avail != 0u) && (firing == 0u) && std::isfinite(fill) && (fill >= 0.99f) && (speed_kmh >= 10.0f);
        }

        // Special Ability (0 = Zone, 1 = Agro, 2 = Roar): show when available and charged/active.
        if (safe_mem::CanAccess(player_car + 0xD4u, 0x28u)) {
            const int32_t stype = static_cast<int32_t>(kernel::R32(player_car + 0xF8u));
            const int32_t sstate = static_cast<int32_t>(kernel::R32(player_car + 0xF4u));
            const uint32_t sfill_bits = kernel::R32(player_car + 0xD4u);
            float sfill = 0.0f;
            std::memcpy(&sfill, &sfill_bits, sizeof(sfill));
            if (stype >= 0 && stype <= 2) {
                special_type = static_cast<int8_t>(stype);
                special_ready = (sstate == 3) || (std::isfinite(sfill) && sfill >= 0.25f);
                special_full = (sstate != 3) && std::isfinite(sfill) && (sfill >= 0.99f);
            }
        }
    }

    tel.has_nos.store(has_nos, std::memory_order_relaxed);
    tel.turbo_ready.store(turbo_ready, std::memory_order_relaxed);
    tel.special_ready.store(special_ready, std::memory_order_relaxed);
    tel.special_full.store(special_full, std::memory_order_relaxed);
    tel.special_type.store(special_type, std::memory_order_relaxed);
    tel.speed_kmh.store(speed_kmh, std::memory_order_relaxed);
    tel.is_bike.store(is_bike, std::memory_order_relaxed);
    tel.race_countdown.store(race_countdown, std::memory_order_relaxed);

    // Sync active in-game Controller Configuration / Custom Remap table so remapped controls
    // automatically mirror onto touch buttons (sub_002693D0 -> [profile_mgr + 0x114 + 0x18], table @ 0x00558848).
    const uint32_t profile_mgr = safe_mem::Read32(0x0053F2E0u, 0u);
    if (profile_mgr != 0u) {
        const uint32_t config_idx = safe_mem::Read32(profile_mgr + 0x12Cu, 0xFFFFFFFFu);
        if (config_idx < 16u) {
            const uint32_t cfg_base = 0x00558848u + config_idx * 0x140u;
            static constexpr uint32_t kMc3dActionId[static_cast<size_t>(GameAction::Count)] = {
                0x00u,  // SteerAxis
                0x02u,  // Gas
                0x03u,  // Brake
                0x07u,  // Handbrake
                0x08u,  // WeightTransfer (WEIGHT_XFER)
                0x09u,  // Nitro
                0x0Au,  // LookBack
                0x0Bu,  // Camera (CAM_UP)
                0x0Du,  // Map
                0x10u,  // FlashLights (HEADLIGHTS)
                0x12u,  // Pause
                0x14u,  // RadioNext (MUSIC_UP)
                0x15u,  // RadioPrev (MUSIC_DOWN)
                0x23u,  // Special (IN_ZONE)
            };
            for (size_t i = 0; i < static_cast<size_t>(GameAction::Count); ++i) {
                uint32_t aid = kMc3dActionId[i];
                uint32_t dev = safe_mem::Read32(cfg_base + aid * 8u, 0u);
                uint32_t key = safe_mem::Read32(cfg_base + aid * 8u + 4u, 0u);
                if (i == static_cast<size_t>(GameAction::Gas) && (dev == 0u || config_idx == 9u)) {
                    const uint32_t alt_dev = safe_mem::Read32(cfg_base + 0x04u * 8u, 0u);
                    const uint32_t alt_key = safe_mem::Read32(cfg_base + 0x04u * 8u + 4u, 0u);
                    if (alt_dev != 0u) {
                        dev = alt_dev;
                        key = alt_key;
                    }
                }
                tel.bindings[i].dev.store(dev, std::memory_order_relaxed);
                tel.bindings[i].key.store(key, std::memory_order_relaxed);
            }
        }
    }
}

void OnFrameTick() {
    UpdateTouchTelemetry();
    ResetCheatsOnSaveSwitch();
    ApplyUnlockFlags();
    ApplyTrafficSettings();
    ApplyArcadeSettings();
    ApplyFullThrottleNos();
}

}  // namespace game::cheats

namespace game {

void UpdateTouchTelemetry() {
    cheats::UpdateTouchTelemetry();
}

void NotifyFmvOpen(const char* raw_name) {
    auto& tel = Telemetry();
    const std::string name = raw_name ? raw_name : "";
    const bool unskippable = cheats::IsUnskippableMovieName(name);
    tel.fmv_unskippable.store(unskippable, std::memory_order_relaxed);
    tel.fmv_frame_ms.store(cheats::SteadyNowMs(), std::memory_order_relaxed);
    cheats::UpdateTouchTelemetry();
    LOGI("[OK] fmv: opened '%s' (%s)", name.c_str(), unskippable ? "unskippable-hide-all" : "skippable-hold-start");
}

void NotifyFmvFrame() {
    Telemetry().fmv_frame_ms.store(cheats::SteadyNowMs(), std::memory_order_relaxed);
    cheats::UpdateTouchTelemetry();
}

}  // namespace game

// Active savegame name setter: __thiscall void sub_000197C0(const char* name) [ret 0x04].
// Reset session cheats cleanly whenever switching or unloading saves.
X86_FUNC(sub_000197C0) {
    __imp__sub_000197C0(ctx, base);
    game::cheats::ResetCheatsOnSaveSwitch();
}

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

// sub_00144C90(__thiscall void AddUpgradeSpentToCarResaleValue(int32_t old_cash)) [ret 0x04].
// Under ZEROCOST, items cost $0 so skip adding (cur_cash - old_cash) to the car's resale price
// (previously wrapping this with 99,999,999 inflated car resale prices to ~100 million!).
X86_FUNC(sub_00144C90) {
    if (game::cheats::g_state.zero_cost.load(std::memory_order_relaxed)) {
        ctx.esp += 8u;
        return;
    }
    __imp__sub_00144C90(ctx, base);
}

X86_FUNC(sub_001474A0) { game::cheats::RunWithZeroCost(ctx, base, __imp__sub_001474A0); }
X86_FUNC(sub_0014E300) { game::cheats::RunWithZeroCost(ctx, base, __imp__sub_0014E300); }

// Native direct-touch Garage Polar Camera orbit & pinch-zoom hook on mcGarage::UpdateCameraOrbit()
// (__thiscall void sub_001442A0() [ret]). Directly updates [cam + 0xA0] (yaw), [cam + 0xA4] (pitch),
// and [garage + 0x92C] (FOV zoom) right before cam->Update(), bypassing Right-Stick analog simulation.
X86_FUNC(sub_001442A0) {
    const uint32_t garage = ctx.ecx;
    float dyaw = 0.0f, dpitch = 0.0f, dzoom = 0.0f;
    const bool has_delta = game::ConsumeGarageTouchOrbit(dyaw, dpitch, dzoom);
    const bool touching = game::Telemetry().garage_orbit_touching.load(std::memory_order_relaxed);

    if (garage && (touching || has_delta)) {
        // Keep MC3D's idle showcase timer [0x004B23F0] reset while touch orbiting
        if (game::safe_mem::CanAccess(0x004B23F0u, 4u)) {
            kernel::W32(0x004B23F0u, 0u);
        }
        // Cancel idle auto-showcase mode ([garage + 0x9FD]) immediately on touch drag
        if (touching && game::safe_mem::CanAccess(garage + 0x9FCu, 2u) &&
            game::safe_mem::Read8(garage + 0x9FDu, 0u) != 0u) {
            *static_cast<uint8_t*>(kernel::HostPtr(garage + 0x9FCu)) = 1u;
            *static_cast<uint8_t*>(kernel::HostPtr(garage + 0x9FDu)) = 0u;
            if (game::safe_mem::CanAccess(garage + 0x708u, 1u)) {
                *static_cast<uint8_t*>(kernel::HostPtr(garage + 0x708u)) = 0u;
            }
        }
    }

    __imp__sub_001442A0(ctx, base);

    if (!garage || !has_delta) return;
    if (game::safe_mem::Read8(garage + 0x708u, 0u) != 0u) return;

    const uint32_t cam = game::safe_mem::Read32(garage + 0x0Cu, 0u);
    if (!cam || !game::safe_mem::CanAccess(cam + 0x84u, 0x28u)) return;

    auto read_f32 = [](uint32_t va, float def) -> float {
        if (!game::safe_mem::CanAccess(va, 4u)) return def;
        const uint32_t bits = kernel::R32(va);
        float v = def;
        std::memcpy(&v, &bits, sizeof(v));
        return std::isfinite(v) ? v : def;
    };
    auto write_f32 = [](uint32_t va, float v) {
        if (!std::isfinite(v) || !game::safe_mem::CanAccess(va, 4u)) return;
        uint32_t bits = 0u;
        std::memcpy(&bits, &v, sizeof(bits));
        kernel::W32(va, bits);
    };

    const uint32_t cam_mode = game::safe_mem::Read32(garage + 0x4D4u, 10u);

    // 1. Direct 1:1 Yaw ([cam + 0xA0]) matching MC3D's exact sign & mode bounds (sub_001442A0:27477-27565)
    if (std::fabs(dyaw) > 1e-6f) {
        const float yaw_sign = (read_f32(0x0044D068u, 1.0f) < 0.0f) ? -1.0f : 1.0f;
        float yaw = read_f32(cam + 0xA0u, 0.0f) + dyaw * yaw_sign;
        if (cam_mode == 10u) {
            constexpr float kPi = 3.14159265f;
            constexpr float kTwoPi = 6.28318530f;
            while (yaw > kPi) yaw -= kTwoPi;
            while (yaw < -kPi) yaw += kTwoPi;
        } else {
            const float base_yaw = read_f32(garage + 0x7A8u, yaw);
            const float span = std::fabs(read_f32(0x0044DB28u, 1.2f));
            yaw = std::clamp(yaw, base_yaw - span, base_yaw + span);
        }
        write_f32(cam + 0xA0u, yaw);
    }

    // 2. Direct 1:1 Pitch ([cam + 0xA4]) with MC3D's exact bounds & floor clearance check (sub_001442A0:27585-27680)
    if (std::fabs(dpitch) > 1e-6f) {
        const float old_pitch = read_f32(cam + 0xA4u, 0.25f);
        float pitch = old_pitch + dpitch;

        const float max_pitch = std::min(read_f32(0x004549FCu, 1.2f), read_f32(0x00459D50u, 1.2f));
        const float min_pitch = (cam_mode == 6u || cam_mode == 7u) ? 0.0f : read_f32(0x00459D54u, 0.02f);
        if (max_pitch >= min_pitch) {
            pitch = std::clamp(pitch, min_pitch, max_pitch);
        }

        const float dist = read_f32(cam + 0x9Cu, 4.0f);
        const uint32_t car_idx = game::safe_mem::Read32(garage + 0x72Cu, 0u);
        if (cam_mode < 20u && car_idx < 4u) {
            const float base_y = read_f32(garage + cam_mode * 12u + 0x5F0u, 0.0f);
            const uint32_t car = game::safe_mem::Read32(garage + car_idx * 4u + 0x714u, 0u);
            const uint32_t node = car ? game::safe_mem::Read32(car + 0x1Cu, 0u) : 0u;
            if (node != 0u) {
                const float floor_y = read_f32(node + 0x38u, 0.0f) + read_f32(0x00459D4Cu, 0.25f);
                if (std::sin(pitch) * dist + base_y <= floor_y) {
                    pitch = old_pitch;
                }
            }
        }
        const float min_guard = read_f32(0x00459D48u, 0.01f);
        if (pitch < min_guard) {
            pitch = read_f32(0x0045537Cu, min_guard);
        }
        write_f32(cam + 0xA4u, pitch);
    }

    // 3. Direct 2-finger pinch-to-zoom ([garage + 0x92C] -> [cam + 0x84])
    if (std::fabs(dzoom) > 1e-6f && game::safe_mem::CanAccess(garage + 0x924u, 0x0Cu)) {
        const float cur_fov = read_f32(garage + 0x92Cu, 45.0f);
        const float min_fov = std::min(read_f32(0x0044A7F0u, 22.0f), read_f32(0x00448E0Cu, 22.0f));
        const float max_fov = read_f32(0x0044CE8Cu, 65.0f);
        if (max_fov > min_fov) {
            const float next_fov = std::clamp(cur_fov - dzoom, min_fov, max_fov);
            write_f32(garage + 0x92Cu, next_fov);
            const float base_fov = read_f32(garage + 0x924u, next_fov);
            const float blend = read_f32(0x0044AAECu, 0.0f);
            write_f32(cam + 0x84u, next_fov + (base_fov - next_fov) * blend);
        }
    }
}

// Full City Map open/close hook (NavSys::SetOpen, __thiscall void sub_000B2F80(bool open) [ret 0x04]).
X86_FUNC(sub_000B2F80) {
    const uint32_t nav_mgr = ctx.ecx;
    const bool open = game::safe_mem::CanAccess(ctx.esp + 4u, 4u) &&
                      (game::safe_mem::Read8(ctx.esp + 4u, 0u) != 0u);
    if (nav_mgr != 0u) {
        game::cheats::g_full_map_obj.store(nav_mgr, std::memory_order_relaxed);
        game::cheats::g_full_map_open.store(open, std::memory_order_relaxed);
    }
    __imp__sub_000B2F80(ctx, base);
}

// Hide Xbox controller button guide prompts whenever touchscreen is active; restore immediately on external controller press.
// FlashSetIntVar: __thiscall void sub_00139520(const char* name, int val) [ret 0x08]
X86_FUNC(sub_00139520) {
    if (ctx.ecx != 0u && game::safe_mem::CanAccess(ctx.esp + 4u, 8u)) {
        const uint32_t name_va = X86_MEM32(ctx.esp + 4u);
        const uint32_t val = X86_MEM32(ctx.esp + 8u);
        bool is_prompt = (name_va == 0x00457C10u);
        bool is_help = (name_va == 0x00457C30u);
        if (!is_prompt && !is_help && name_va != 0u && game::safe_mem::CanAccess(name_va, 10u)) {
            const uint8_t ch0 = game::safe_mem::Read8(name_va, 0u);
            if (ch0 == 'S') {
                const std::string name = game::safe_mem::ReadSanitizedCString(name_va, 32u);
                if (name == "ShowPrompt") is_prompt = true;
                else if (name == "ShowTxtPromptHelp") is_help = true;
            }
        }
        if (is_prompt || is_help) {
            game::cheats::RecordPromptInt(ctx.ecx, is_prompt, val);
            if (input::TouchActive()) {
                X86_MEM32(ctx.esp + 8u) = 0u;
            }
        }
    }
    __imp__sub_00139520(ctx, base);
}

// FlashSetStringVar: __thiscall void sub_001395E0(const char* name, const char* str) [ret 0x08]
// Hides Full Map & Save Screen Xbox button prompt text labels (txtPrompt*) while using touchscreen.
X86_FUNC(sub_001395E0) {
    if (ctx.ecx != 0u && game::safe_mem::CanAccess(ctx.esp + 4u, 8u)) {
        const uint32_t name_va = X86_MEM32(ctx.esp + 4u);
        const uint32_t str_va = X86_MEM32(ctx.esp + 8u);
        if (name_va != 0u && game::safe_mem::CanAccess(name_va, 10u) &&
            game::safe_mem::Read8(name_va, 0u) == 't') {
            const std::string name = game::safe_mem::ReadSanitizedCString(name_va, 40u);
            if (name.rfind("txtPrompt", 0) == 0) {
                const uint32_t nlen = uint32_t(name.size());
                game::cheats::RecordPromptString(ctx.ecx, name_va, nlen, str_va);
                if (input::TouchActive() && game::safe_mem::CanAccess(name_va + nlen, 1u)) {
                    X86_MEM32(ctx.esp + 8u) = name_va + nlen;  // points to '\0' in .rdata ("")
                }
            }
        }
    }
    __imp__sub_001395E0(ctx, base);
}
