#include "game/cheats_hook.h"
#include "game/touch_context.h"

#include "app/app.h"
#include "core/log.h"
#include "cpu/x86_ops.h"
#include "game/guest_mem_safe.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

#include <algorithm>
#include <atomic>
#include <cctype>
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
X86_EXTERN_FUNC(__imp__sub_00144C90);
X86_EXTERN_FUNC(__imp__sub_001474A0);
X86_EXTERN_FUNC(__imp__sub_0014E300);

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
constexpr uint32_t kGameStateAddr = 0x0053F2D4u;

constexpr uint32_t CheatBit(CheatId id) {
    return 1u << static_cast<uint32_t>(id);
}

struct CheatState {
    std::atomic<bool> full_throttle{false};
    std::atomic<bool> secret_dealer{false};
    std::atomic<bool> no_traffic{false};
    std::atomic<bool> zero_cost{false};
    std::atomic<bool> arcade_powerups{false};
    std::atomic<bool> delivery{false};
    std::atomic<uint32_t> unlocked_mask{0u};

    bool powerup_hud_loaded_by_cheat = false;
};

CheatState g_state;
thread_local int g_zero_cost_depth = 0;

struct SaveCheatRecord {
    std::string savefile;
    uint32_t activated_mask = 0u;
    uint32_t typed_mask = 0u;
};

std::mutex g_db_mutex;
bool g_db_loaded = false;
std::vector<SaveCheatRecord> g_db_records;
std::string g_synced_save_name;

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

std::filesystem::path CheatDbPath() {
    return app::ExeDir() / "saves_cheat.db";
}

// Minimal JSON string escaper/parser for saves_cheat.db:
// [{"savefile": "player1", "activated_cheat": ["ZEROCOST"], "typed_cheat": ["ZEROCOST", "AMSSRBLHGSHARK"]}]
std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 4);
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c >= 0x20u && c < 0x7Fu) out.push_back(static_cast<char>(c));
                break;
        }
    }
    return out;
}

struct JsonReader {
    const std::string& s;
    size_t pos = 0;

    void SkipWs() {
        while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos]))) ++pos;
    }

    bool Consume(char ch) {
        SkipWs();
        if (pos < s.size() && s[pos] == ch) {
            ++pos;
            return true;
        }
        return false;
    }

    bool ParseString(std::string& out) {
        SkipWs();
        if (pos >= s.size() || s[pos] != '"') return false;
        ++pos;
        out.clear();
        while (pos < s.size()) {
            char c = s[pos++];
            if (c == '"') return true;
            if (c == '\\' && pos < s.size()) {
                char esc = s[pos++];
                switch (esc) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    default: out.push_back(esc); break;
                }
            } else {
                out.push_back(c);
            }
        }
        return false;
    }

    bool ParseStringArray(std::vector<std::string>& out) {
        out.clear();
        if (!Consume('[')) return false;
        if (Consume(']')) return true;
        for (;;) {
            std::string item;
            if (!ParseString(item)) return false;
            out.push_back(std::move(item));
            if (Consume(',')) continue;
            return Consume(']');
        }
    }

    void SkipValue() {
        SkipWs();
        if (pos >= s.size()) return;
        char c = s[pos];
        if (c == '"') {
            std::string tmp;
            ParseString(tmp);
            return;
        }
        if (c == '[' || c == '{') {
            char open = c;
            char close = (c == '[') ? ']' : '}';
            int depth = 0;
            bool in_str = false;
            while (pos < s.size()) {
                char ch = s[pos++];
                if (in_str) {
                    if (ch == '\\' && pos < s.size()) ++pos;
                    else if (ch == '"') in_str = false;
                } else {
                    if (ch == '"') in_str = true;
                    else if (ch == open) ++depth;
                    else if (ch == close && --depth <= 0) return;
                }
            }
            return;
        }
        while (pos < s.size() && s[pos] != ',' && s[pos] != '}' && s[pos] != ']') ++pos;
    }
};

void LoadCheatDbLocked() {
    if (g_db_loaded) return;
    g_db_loaded = true;
    g_db_records.clear();

    const auto path = CheatDbPath();
    std::ifstream f(path);
    if (!f) return;
    std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    JsonReader r{data};
    if (!r.Consume('[')) return;
    if (r.Consume(']')) return;

    while (r.pos < data.size()) {
        if (!r.Consume('{')) break;
        SaveCheatRecord rec;
        if (!r.Consume('}')) {
            for (;;) {
                std::string key;
                if (!r.ParseString(key)) break;
                if (!r.Consume(':')) break;
                if (key == "savefile") {
                    r.ParseString(rec.savefile);
                } else if (key == "activated_cheat") {
                    std::vector<std::string> arr;
                    if (r.ParseStringArray(arr)) {
                        for (const auto& name : arr) {
                            CheatId id;
                            if (ParseCheatName(name, &id)) {
                                rec.typed_mask |= CheatBit(id);
                                if (id != CheatId::Cash5000) {
                                    rec.activated_mask |= CheatBit(id);
                                }
                            }
                        }
                    }
                } else if (key == "typed_cheat") {
                    std::vector<std::string> arr;
                    if (r.ParseStringArray(arr)) {
                        for (const auto& name : arr) {
                            CheatId id;
                            if (ParseCheatName(name, &id)) {
                                rec.typed_mask |= CheatBit(id);
                            }
                        }
                    }
                } else {
                    r.SkipValue();
                }
                if (r.Consume(',')) continue;
                r.Consume('}');
                break;
            }
        }
        rec.savefile = TrimString(rec.savefile);
        rec.typed_mask |= rec.activated_mask;
        if (!rec.savefile.empty()) {
            g_db_records.push_back(std::move(rec));
        }
        if (r.Consume(',')) continue;
        r.Consume(']');
        break;
    }
    LOGI("[OK] cheats: loaded saves_cheat.db (%zu save entries)", g_db_records.size());
}

void AppendCheatArrayJson(std::string& out, uint32_t mask, bool include_cash) {
    out += "[";
    bool first = true;
    for (int i = 0; i < static_cast<int>(CheatId::Count); ++i) {
        const auto id = static_cast<CheatId>(i);
        if (!include_cash && id == CheatId::Cash5000) continue;
        if ((mask & CheatBit(id)) == 0u) continue;
        if (!first) out += ", ";
        first = false;
        out += "\"";
        out += GetCheatCodeName(id);
        out += "\"";
    }
    out += "]";
}

void WriteCheatDbLocked() {
    const auto path = CheatDbPath();
    auto tmp = path;
    tmp += ".tmp";

    std::string out = "[\n";
    for (size_t i = 0; i < g_db_records.size(); ++i) {
        const auto& rec = g_db_records[i];
        out += "  {\n";
        out += "    \"savefile\": \"" + JsonEscape(rec.savefile) + "\",\n";
        out += "    \"activated_cheat\": ";
        AppendCheatArrayJson(out, rec.activated_mask, false);
        out += ",\n";
        out += "    \"typed_cheat\": ";
        AppendCheatArrayJson(out, rec.typed_mask | rec.activated_mask, true);
        out += "\n  }";
        if (i + 1 < g_db_records.size()) out += ",";
        out += "\n";
    }
    out += "]\n";

    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            LOGE("[ERR] cheats: failed to write %s", tmp.c_str());
            return;
        }
        f.write(out.data(), static_cast<std::streamsize>(out.size()));
        f.flush();
        if (!f) return;
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::filesystem::remove(path, ec);
        ec.clear();
        std::filesystem::rename(tmp, path, ec);
    }
}

uint32_t CurrentActivatedMask() {
    uint32_t m = 0u;
    if (g_state.full_throttle.load(std::memory_order_relaxed)) m |= CheatBit(CheatId::FullThrottle);
    if (g_state.secret_dealer.load(std::memory_order_relaxed)) m |= CheatBit(CheatId::SecretDealer);
    if (g_state.no_traffic.load(std::memory_order_relaxed)) m |= CheatBit(CheatId::NoTraffic);
    if (g_state.zero_cost.load(std::memory_order_relaxed)) m |= CheatBit(CheatId::ZeroCost);
    if (g_state.arcade_powerups.load(std::memory_order_relaxed)) m |= CheatBit(CheatId::Arcade);
    if (g_state.delivery.load(std::memory_order_relaxed)) m |= CheatBit(CheatId::Delivery);
    return m;
}

void ApplyMasksToLiveState(uint32_t activated_mask, uint32_t typed_mask) {
    g_state.full_throttle.store((activated_mask & CheatBit(CheatId::FullThrottle)) != 0u, std::memory_order_relaxed);
    g_state.secret_dealer.store((activated_mask & CheatBit(CheatId::SecretDealer)) != 0u, std::memory_order_relaxed);
    g_state.no_traffic.store((activated_mask & CheatBit(CheatId::NoTraffic)) != 0u, std::memory_order_relaxed);
    g_state.zero_cost.store((activated_mask & CheatBit(CheatId::ZeroCost)) != 0u, std::memory_order_relaxed);
    g_state.arcade_powerups.store((activated_mask & CheatBit(CheatId::Arcade)) != 0u, std::memory_order_relaxed);
    g_state.delivery.store((activated_mask & CheatBit(CheatId::Delivery)) != 0u, std::memory_order_relaxed);
    g_state.unlocked_mask.store(typed_mask | activated_mask, std::memory_order_relaxed);
}

std::string ReadGuestActiveSaveName() {
    const uint32_t mgr = safe_mem::Read32(kSaveMgrGlobalPtrAddr, 0u);
    if (!mgr) return {};
    if (!safe_mem::CanAccess(mgr + kSaveMgrNameOffset, 4u)) return {};
    return TrimString(safe_mem::ReadSanitizedCString(mgr + kSaveMgrNameOffset, 64));
}

void PersistCurrentSaveStateLocked(const std::string& save_name) {
    if (save_name.empty()) return;
    LoadCheatDbLocked();
    const uint32_t active_mask = CurrentActivatedMask();
    const uint32_t typed_mask = g_state.unlocked_mask.load(std::memory_order_relaxed) | active_mask;

    auto it = std::find_if(g_db_records.begin(), g_db_records.end(),
                           [&](const SaveCheatRecord& r) { return r.savefile == save_name; });
    if (it != g_db_records.end()) {
        it->activated_mask = active_mask;
        it->typed_mask = typed_mask;
    } else if (typed_mask != 0u || active_mask != 0u) {
        g_db_records.push_back(SaveCheatRecord{save_name, active_mask, typed_mask});
    } else {
        return;
    }
    WriteCheatDbLocked();
}

void SyncWithActiveSaveIfChanged() {
    const std::string active_save = ReadGuestActiveSaveName();
    std::lock_guard<std::mutex> lock(g_db_mutex);
    LoadCheatDbLocked();
    if (active_save == g_synced_save_name) return;

    const std::string prev_save = g_synced_save_name;
    g_synced_save_name = active_save;

    if (active_save.empty()) {
        // Save unloaded / back to fresh state: clear active & unlocked cheats so next save starts clean.
        ApplyMasksToLiveState(0u, 0u);
        LOGI("[OK] cheats: active save cleared (was '%s'), reset cheat state", prev_save.c_str());
        return;
    }

    auto it = std::find_if(g_db_records.begin(), g_db_records.end(),
                           [&](const SaveCheatRecord& r) { return r.savefile == active_save; });
    if (it != g_db_records.end()) {
        ApplyMasksToLiveState(it->activated_mask, it->typed_mask);
        LOGI("[OK] cheats: synced save '%s' from saves_cheat.db (active=0x%X typed=0x%X)",
             active_save.c_str(), it->activated_mask, it->typed_mask);
    } else if (prev_save.empty() && g_state.unlocked_mask.load(std::memory_order_relaxed) != 0u) {
        // Brand-new career saved for the first time after typing cheats in this unsaved session.
        PersistCurrentSaveStateLocked(active_save);
        LOGI("[OK] cheats: bound unsaved session cheats to new save '%s'", active_save.c_str());
    } else {
        // Switched to a different save (e.g. player2) with no saved cheats: start completely clean!
        ApplyMasksToLiveState(0u, 0u);
        LOGI("[OK] cheats: switched to clean save '%s' (no saved cheats)", active_save.c_str());
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

void MarkUnlockedAndPersist(CheatId id) {
    g_state.unlocked_mask.fetch_or(CheatBit(id), std::memory_order_relaxed);
    const std::string active_save = ReadGuestActiveSaveName();
    std::lock_guard<std::mutex> lock(g_db_mutex);
    if (!active_save.empty()) {
        g_synced_save_name = active_save;
        PersistCurrentSaveStateLocked(active_save);
    }
}

bool TryHandleCheatCode(const std::string& raw) {
    SyncWithActiveSaveIfChanged();
    CheatId id;
    if (!ParseCheatName(raw, &id)) return false;

    if (id == CheatId::Cash5000) {
        const bool ok = AddCash(5000);
        MarkUnlockedAndPersist(id);
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
    MarkUnlockedAndPersist(id);
    LOGI("[OK] cheats: %s -> %s (save='%s')", GetCheatCodeName(id), on ? "ON" : "OFF",
         ReadGuestActiveSaveName().c_str());
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

void UpdateTouchTelemetry() {
    auto& tel = game::Telemetry();

    const uint32_t state_mgr = safe_mem::Read32(0x00557ED0u, 0u);
    const uint32_t app_state = state_mgr ? safe_mem::Read32(state_mgr + 0x04u, 0u) : 0u;

    const uint32_t menu_mgr = safe_mem::Read32(0x0050F148u, 0u);
    const uint32_t menu_obj = menu_mgr ? safe_mem::Read32(menu_mgr + 0x0Cu, 0u) : 0u;
    const uint32_t menu_owner = menu_obj ? safe_mem::Read32(menu_obj + 0x0Cu, 0u) : 0u;
    const bool menu_visible = menu_owner && ((safe_mem::Read8(menu_owner + 0x44u, 0u) & 0x01u) != 0u);

    const uint32_t car_mgr = safe_mem::Read32(0x00558814u, 0u);
    const uint32_t player_car = car_mgr ? safe_mem::Read32(car_mgr + 0x08u, 0u) : 0u;

    UiContext ctx = UiContext::MainMenu;
    if (app_state == 0x03u) {
        ctx = UiContext::Garage;
    } else if (app_state == 0x06u) {
        ctx = UiContext::Loading;
    } else if (app_state == 0x02u) {
        if (menu_visible) ctx = UiContext::MainMenu;
        else if (player_car != 0u) ctx = UiContext::Gameplay;
        else ctx = UiContext::Loading;
    } else {
        ctx = UiContext::MainMenu;
    }
    tel.ui_context.store(static_cast<uint8_t>(ctx), std::memory_order_relaxed);

    bool has_nos = false;
    bool turbo_ready = false;
    bool special_ready = false;
    int8_t special_type = -1;
    float speed_kmh = 0.0f;

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
            }
        }
    }

    tel.has_nos.store(has_nos, std::memory_order_relaxed);
    tel.turbo_ready.store(turbo_ready, std::memory_order_relaxed);
    tel.special_ready.store(special_ready, std::memory_order_relaxed);
    tel.special_type.store(special_type, std::memory_order_relaxed);
    tel.speed_kmh.store(speed_kmh, std::memory_order_relaxed);
}

void OnFrameTick() {
    UpdateTouchTelemetry();
    SyncWithActiveSaveIfChanged();
    ApplyUnlockFlags();
    ApplyTrafficSettings();
    ApplyArcadeSettings();
    ApplyFullThrottleNos();
}

void ReloadCheatDb() {
    {
        std::lock_guard<std::mutex> lock(g_db_mutex);
        g_db_loaded = false;
        g_synced_save_name.clear();
    }
    SyncWithActiveSaveIfChanged();
}

bool HasAnyUnlockedCheat() {
    SyncWithActiveSaveIfChanged();
    return g_state.unlocked_mask.load(std::memory_order_relaxed) != 0u;
}

bool IsCheatUnlocked(CheatId id) {
    SyncWithActiveSaveIfChanged();
    return (g_state.unlocked_mask.load(std::memory_order_relaxed) & CheatBit(id)) != 0u;
}

bool IsCheatActive(CheatId id) {
    auto* flag = FlagForCheat(id);
    return flag ? flag->load(std::memory_order_relaxed) : false;
}

bool ToggleCheat(CheatId id) {
    SyncWithActiveSaveIfChanged();
    auto* flag = FlagForCheat(id);
    if (!flag) return false;
    const bool on = !flag->load(std::memory_order_relaxed);
    flag->store(on, std::memory_order_relaxed);
    if (on) {
        if (id == CheatId::NoTraffic) ApplyTrafficSettings();
        else if (id == CheatId::Arcade) ApplyArcadeSettings();
        else if (id == CheatId::Delivery) ApplyUnlockFlags();
    }
    MarkUnlockedAndPersist(id);
    LOGI("[OK] cheats: menu toggled %s -> %s", GetCheatCodeName(id), on ? "ON" : "OFF");
    return on;
}

bool TriggerCashCheat() {
    SyncWithActiveSaveIfChanged();
    const bool ok = AddCash(5000);
    LOGI("[OK] cheats: menu clicked AMSSRBLHGSHARK -> +5000 cash (%s)", ok ? "applied" : "no-profile");
    return ok;
}

int32_t GetCurrentCash() {
    const uint32_t prof = GetProfilePtr();
    if (!prof || !safe_mem::CanAccess(prof + kProfileCashOffset, 4u)) return -1;
    return static_cast<int32_t>(kernel::R32(prof + kProfileCashOffset));
}

std::string GetActiveSaveName() {
    return ReadGuestActiveSaveName();
}

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

const char* GetCheatMenuLabel(CheatId id) {
    switch (id) {
        case CheatId::Cash5000:     return "Add $5,000 Cash";
        case CheatId::FullThrottle: return "Full Throttle (NOS)";
        case CheatId::SecretDealer: return "Secret Dealer (REMIX)";
        case CheatId::NoTraffic:    return "Ghost Town (No Traffic)";
        case CheatId::ZeroCost:     return "Zero Cost (Free Shop)";
        case CheatId::Arcade:       return "Arcade Powerups";
        case CheatId::Delivery:     return "Delivery (All Cities)";
        default:                    return "Cheat";
    }
}

}  // namespace game::cheats

// Active savegame name setter: __thiscall void sub_000197C0(const char* name) [ret 0x04].
// Immediately sync saves_cheat.db whenever a savefile is loaded or saved.
X86_FUNC(sub_000197C0) {
    __imp__sub_000197C0(ctx, base);
    game::cheats::SyncWithActiveSaveIfChanged();
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
