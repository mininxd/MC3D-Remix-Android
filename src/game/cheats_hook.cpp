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
X86_EXTERN_FUNC(__imp__sub_000659D0);
X86_EXTERN_FUNC(__imp__sub_00209000);
X86_EXTERN_FUNC(__imp__sub_002090A0);
X86_EXTERN_FUNC(__imp__sub_002C4A10);
X86_EXTERN_FUNC(__imp__sub_001DB0D0);
X86_EXTERN_FUNC(__imp__sub_001DB557);
X86_EXTERN_FUNC(__imp__sub_00206400);
X86_EXTERN_FUNC(__imp__sub_002071C0);
X86_EXTERN_FUNC(__imp__sub_00350120);

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
constexpr uint32_t kNativeSpecialAddr = 0x004B361Cu;
constexpr uint32_t kNativeHeadAddr = 0x004B39D8u;
constexpr uint32_t kNativeNoDamageAddr = 0x00509F80u;
constexpr uint32_t kNativeFireHeadAddr = 0x00557F08u;
constexpr uint32_t kNativeHyperAgroAddr = 0x00558818u;
constexpr uint32_t kHeadFireHwVal = 0xFFFFFFFEu;
constexpr uint32_t kVehicleListPtrAddr = 0x00557EB0u;
constexpr uint32_t kVehicleListCountAddr = 0x00557EB4u;

}  // namespace

std::atomic<uint32_t> g_full_map_obj{0};
std::atomic<bool> g_full_map_open{false};
std::atomic<uint32_t> g_button_prompts_table_va{0};
std::atomic<uint64_t> g_flash_challenge_ms{0};

bool IsControllerPromptClip(uint32_t name_va) {
    if (!name_va || !safe_mem::CanAccess(name_va, 2u)) return false;
    const auto* src = static_cast<const char*>(kernel::HostPtr(name_va));
    char low[36];
    size_t len = 0;
    for (; len < 35u; ++len) {
        if (!safe_mem::CanAccess(name_va + static_cast<uint32_t>(len), 1u)) break;
        const unsigned char c = static_cast<unsigned char>(src[len]);
        if (c == 0u) break;
        if (c < 0x20u || c >= 0x7Fu) return false;
        low[len] = static_cast<char>((c >= 'A' && c <= 'Z') ? (c + ('a' - 'A')) : c);
    }
    low[len] = '\0';
    if (len < 4u) return false;
    if (std::strstr(low, "prompt") != nullptr) return true;
    return std::strcmp(low, "triggers") == 0 ||
           std::strcmp(low, "btn_tri") == 0 ||
           std::strcmp(low, "rstick") == 0 ||
           std::strcmp(low, "slotok") == 0 ||
           std::strcmp(low, "slotback") == 0 ||
           std::strcmp(low, "slotchangerace") == 0 ||
           std::strcmp(low, "slotnavsys") == 0 ||
           std::strcmp(low, "slottutorials") == 0;
}

void SyncPromptVisibility(X86Context&, uint8_t*) {
    // Visibility of controller prompts (both button icons and text labels) is handled cleanly
    // at draw time in sub_000659D0 / sub_002090A0 / sub_002C4A10 so Flash string/int variables
    // stay 100% intact when switching between touchscreen and an external controller.
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

enum class CollectBit : int {
    Cash5000 = 0,
    FullThrottle = 1,
    SecretDealer = 2,
    NoTraffic = 3,
    ZeroCost = 4,
    Arcade = 5,
    Delivery = 6,
    Roadtrip = 7,
    NoDamage = 8,
    HeadFire = 9,
    HyperAgro = 10,
    CashPlus1 = 11,
    CashMinus1 = 12,
    SpecialZone = 13,
    SpecialAgro = 14,
    SpecialRoar = 15,
    HeadSmiley = 16,
    HeadPumpkin = 17,
    HeadBunny = 18,
    HeadSnowman = 19,
    Count = 20,
};

struct CollectEntryDef {
    CollectBit bit;
    const char* code;
};

constexpr CollectEntryDef kCollectEntries[] = {
    {CollectBit::Cash5000, "amssrblhgshark"},
    {CollectBit::FullThrottle, "fullthrottle"},
    {CollectBit::SecretDealer, "secretdealer"},
    {CollectBit::NoTraffic, "ghosttown"},
    {CollectBit::ZeroCost, "zerocost"},
    {CollectBit::Arcade, "arcade"},
    {CollectBit::Delivery, "delivery"},
    {CollectBit::Roadtrip, "roadtrip"},
    {CollectBit::NoDamage, "ontheroad"},
    {CollectBit::HeadFire, "trythisathome"},
    {CollectBit::HyperAgro, "hyperagro"},
    {CollectBit::CashPlus1, "kubmir"},
    {CollectBit::CashMinus1, "rimbuk"},
    {CollectBit::SpecialZone, "allin"},
    {CollectBit::SpecialAgro, "dfens"},
    {CollectBit::SpecialRoar, "Rjnr"},
    {CollectBit::HeadSmiley, "getheadj"},
    {CollectBit::HeadPumpkin, "getheadk"},
    {CollectBit::HeadBunny, "getheadl"},
    {CollectBit::HeadSnowman, "getheadm"},
};

struct CheatState {
    std::atomic<bool> full_throttle{false};
    std::atomic<bool> secret_dealer{false};
    std::atomic<bool> no_traffic{false};
    std::atomic<bool> zero_cost{false};
    std::atomic<bool> arcade_powerups{false};
    std::atomic<bool> delivery{false};
    std::atomic<bool> native_roadtrip{false};

    bool powerup_hud_loaded_by_cheat = false;
    bool saved_traffic_valid = false;
    uint32_t saved_traffic_1 = 4u;
    uint32_t saved_traffic_2 = 4u;
};

CheatState g_state;
thread_local int g_zero_cost_depth = 0;

std::mutex g_save_mutex;
std::string g_synced_save_name;
std::atomic<bool> g_legacy_db_cleaned{false};

std::mutex g_collection_mutex;
std::atomic<bool> g_collection_loaded{false};
std::atomic<uint32_t> g_collected_mask{0u};

constexpr int kRowIdBase = 1000;
enum CollectionRowId : int {
    kRowEmpty = kRowIdBase,
    kRowCash5000,
    kRowCashPlus1,
    kRowCashMinus1,
    kRowFullThrottle,
    kRowSecretDealer,
    kRowNoTraffic,
    kRowZeroCost,
    kRowArcade,
    kRowDelivery,
    kRowRoadtrip,
    kRowNoDamage,
    kRowHyperAgro,
    kRowSpecialSingle,
    kRowSpecialGroup,
    kRowHeadSingle,
    kRowHeadGroup,
    kRowIdEnd,
};

constexpr uint32_t BitMask(CollectBit b) {
    return 1u << static_cast<int>(b);
}

void WriteByteFlag(uint32_t addr, uint8_t value) {
    if (safe_mem::CanAccess(addr, 1u)) {
        *static_cast<uint8_t*>(kernel::HostPtr(addr)) = value;
    }
}

void SyncUnlockAllCitiesFlags() {
    const uint8_t flag = (g_state.delivery.load(std::memory_order_relaxed) ||
                          g_state.native_roadtrip.load(std::memory_order_relaxed)) ? 1u : 0u;
    WriteByteFlag(kUnlockAllCitiesFlag1, flag);
    WriteByteFlag(kUnlockAllCitiesFlag2, flag);
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

CollectBit BitForCustomCheat(CheatId id) {
    switch (id) {
        case CheatId::Cash5000:     return CollectBit::Cash5000;
        case CheatId::FullThrottle: return CollectBit::FullThrottle;
        case CheatId::SecretDealer: return CollectBit::SecretDealer;
        case CheatId::NoTraffic:    return CollectBit::NoTraffic;
        case CheatId::ZeroCost:     return CollectBit::ZeroCost;
        case CheatId::Arcade:       return CollectBit::Arcade;
        case CheatId::Delivery:     return CollectBit::Delivery;
        default:                    return CollectBit::Cash5000;
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

// Mirrors native sub_00192350 cipher: c_enc = ((c + esi - 0x41) % 0x3A) + 0x41, esi = 0x45 + idx.
std::string ObfuscateNativeCheat(const std::string& raw) {
    const std::string s = TrimString(raw);
    std::string out;
    out.reserve(s.size());
    uint32_t esi = 0x45u;
    for (unsigned char uc : s) {
        const uint32_t val = static_cast<uint32_t>(uc) + esi - 0x41u;
        const char ch = static_cast<char>((val % 0x3Au) + 0x41u);
        out.push_back(ch);
        ++esi;
    }
    return out;
}

bool ParseNativeCheatCode(const std::string& raw, CollectBit* out_bit) {
    struct NativeEncEntry {
        const char* enc;
        CollectBit bit;
    };
    static constexpr NativeEncEntry kTable[] = {
        {"CAnrIHzH", CollectBit::Roadtrip},               // roadtrip
        {"nDBGHsFMGNMU", CollectBit::Roadtrip},           // crosscountry
        {"FDooCIGJtQG", CollectBit::Roadtrip},            // urbansprawl
        {"sKCsGqxJH", CollectBit::HyperAgro},             // hyperagro
        {"orrBH", CollectBit::SpecialAgro},               // dfens
        {"lxywC", CollectBit::SpecialZone},               // allin
        {"]vAF", CollectBit::SpecialRoar},                // Rjnr
        {"zzGvtHFsw", CollectBit::NoDamage},              // ontheroad
        {"CuzpJA", CollectBit::CashMinus1},               // rimbuk
        {"vGoAxH", CollectBit::CashPlus1},                // kubmir
        {"EDLHwyJsMBJIB", CollectBit::HeadFire},          // trythisathome
        {"rqGvtquB", CollectBit::HeadSmiley},             // getheadj / gethead0
        {"rqGvtquC", CollectBit::HeadPumpkin},            // getheadk / gethead1
        {"rqGvtquD", CollectBit::HeadBunny},              // getheadl / gethead2
        {"rqGvtquE", CollectBit::HeadSnowman},            // getheadm / gethead3
    };
    const std::string enc = ObfuscateNativeCheat(raw);
    for (const auto& entry : kTable) {
        if (enc == entry.enc) {
            *out_bit = entry.bit;
            return true;
        }
    }
    return false;
}

std::filesystem::path HiddenCollectionPath() {
    return app::ExeDir() / "shadercache" / "shader_stages.bin";
}

uint32_t ParseCollectionStream(std::istream& in) {
    uint32_t mask = 0u;
    std::string line;
    while (std::getline(in, line)) {
        const std::string code = TrimString(line);
        if (code.empty() || code[0] == '#') continue;
        for (const auto& e : kCollectEntries) {
            if (code == e.code) {
                mask |= BitMask(e.bit);
                break;
            }
        }
    }
    return mask;
}

void SaveCollectionLocked(uint32_t mask);

uint32_t ReadCollectionFromDiskLocked() {
    const auto hidden_path = HiddenCollectionPath();
    std::ifstream in(hidden_path);
    if (in.is_open()) {
        return ParseCollectionStream(in);
    }

    // Silently migrate and remove older plaintext cheat_collection.db if present.
    const auto legacy_path = app::ExeDir() / "cheat_collection.db";
    std::ifstream legacy_in(legacy_path);
    if (!legacy_in.is_open()) return 0u;
    const uint32_t mask = ParseCollectionStream(legacy_in);
    legacy_in.close();
    SaveCollectionLocked(mask);
    std::error_code ec;
    std::filesystem::remove(legacy_path, ec);
    auto legacy_tmp = legacy_path;
    legacy_tmp += ".tmp";
    std::filesystem::remove(legacy_tmp, ec);
    return mask;
}

uint32_t EnsureCollectionLoaded() {
    if (g_collection_loaded.load(std::memory_order_acquire)) {
        return g_collected_mask.load(std::memory_order_relaxed);
    }
    std::lock_guard<std::mutex> lock(g_collection_mutex);
    if (!g_collection_loaded.load(std::memory_order_relaxed)) {
        g_collected_mask.store(ReadCollectionFromDiskLocked(), std::memory_order_relaxed);
        g_collection_loaded.store(true, std::memory_order_release);
    }
    return g_collected_mask.load(std::memory_order_relaxed);
}

void SaveCollectionLocked(uint32_t mask) {
    const auto db_path = HiddenCollectionPath();
    std::error_code dir_ec;
    std::filesystem::create_directories(db_path.parent_path(), dir_ec);
    auto tmp_path = db_path;
    tmp_path += ".tmp";
    {
        std::ofstream out(tmp_path, std::ios::trunc);
        if (!out.is_open()) {
            LOGE("[ERR] cheats: failed to open %s for writing", tmp_path.c_str());
            return;
        }
        for (const auto& e : kCollectEntries) {
            if ((mask & BitMask(e.bit)) != 0u) {
                out << e.code << '\n';
            }
        }
        out.flush();
        if (!out.good()) {
            LOGE("[ERR] cheats: failed writing %s", tmp_path.c_str());
            return;
        }
    }
    std::error_code ec;
    std::filesystem::rename(tmp_path, db_path, ec);
    if (ec) {
        std::error_code ec2;
        std::filesystem::remove(db_path, ec2);
        ec.clear();
        std::filesystem::rename(tmp_path, db_path, ec);
    }
}

void RecordCollectedBit(CollectBit bit) {
    const uint32_t add = BitMask(bit);
    std::lock_guard<std::mutex> lock(g_collection_mutex);
    if (!g_collection_loaded.load(std::memory_order_relaxed)) {
        g_collected_mask.store(ReadCollectionFromDiskLocked(), std::memory_order_relaxed);
        g_collection_loaded.store(true, std::memory_order_release);
    }
    const uint32_t prev = g_collected_mask.load(std::memory_order_relaxed);
    const uint32_t next = prev | add;
    if (next != prev) {
        g_collected_mask.store(next, std::memory_order_relaxed);
        SaveCollectionLocked(next);
    }
}

std::atomic<bool> g_back_delete_hold[2] = {false, false};
std::atomic<uint64_t> g_back_delete_cooldown_ms{0};
thread_local bool t_back_forced_backspace = false;

uint32_t FindActiveKeyboardVa(bool* out_has_text) {
    if (out_has_text) *out_has_text = false;
    const uint32_t mgr = safe_mem::Read32(0x0050F148u, 0u);
    const uint32_t host = mgr ? safe_mem::Read32(mgr + 0x0Cu, 0u) : 0u;
    const uint32_t kbd = host ? safe_mem::Read32(host + 0x130u, 0u) : 0u;
    if (!kbd || !safe_mem::CanAccess(kbd, 0x240u)) return 0u;
    if (safe_mem::Read32(kbd, 0u) != 0x004A3A38u) return 0u;
    if ((safe_mem::Read8(kbd + 0x44u, 0u) & 0x01u) == 0u) return 0u;
    if (safe_mem::Read8(kbd + 0x4Bu, 0u) != 0u || safe_mem::Read8(kbd + 0x4Cu, 0u) != 0u) return 0u;
    if (out_has_text) {
        const uint32_t buf_va = safe_mem::Read32(kbd + 0x23Cu, 0u);
        *out_has_text = (buf_va != 0u && safe_mem::Read8(buf_va, 0u) != 0u);
    }
    return kbd;
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
    g_state.native_roadtrip.store(false, std::memory_order_relaxed);
    if (safe_mem::CanAccess(kNativeSpecialAddr, 4u)) kernel::W32(kNativeSpecialAddr, 0xFFFFFFFFu);
    if (safe_mem::CanAccess(kNativeHeadAddr, 4u)) kernel::W32(kNativeHeadAddr, 0xFFFFFFFFu);
    WriteByteFlag(kNativeNoDamageAddr, 0u);
    WriteByteFlag(kNativeFireHeadAddr, 0u);
    WriteByteFlag(kNativeHyperAgroAddr, 0u);
    WriteByteFlag(kUnlockAllCitiesFlag1, 0u);
    WriteByteFlag(kUnlockAllCitiesFlag2, 0u);
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
    if (!g_state.saved_traffic_valid) {
        const uint32_t t1 = safe_mem::Read32(kTrafficGlobal1, 4u);
        const uint32_t t2 = safe_mem::Read32(kTrafficGlobal2, 4u);
        g_state.saved_traffic_1 = t1 ? t1 : 4u;
        g_state.saved_traffic_2 = t2 ? t2 : 4u;
        g_state.saved_traffic_valid = true;
    }
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

void RestoreTrafficSettings() {
    const uint32_t t1 = g_state.saved_traffic_valid ? g_state.saved_traffic_1 : 4u;
    const uint32_t t2 = g_state.saved_traffic_valid ? g_state.saved_traffic_2 : 4u;
    if (safe_mem::CanAccess(kTrafficGlobal1, 4u)) kernel::W32(kTrafficGlobal1, t1);
    if (safe_mem::CanAccess(kTrafficGlobal2, 4u)) kernel::W32(kTrafficGlobal2, t2);
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
    if (g_state.delivery.load(std::memory_order_relaxed) ||
        g_state.native_roadtrip.load(std::memory_order_relaxed)) {
        SyncUnlockAllCitiesFlags();
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

    RecordCollectedBit(BitForCustomCheat(id));

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
    } else {
        if (id == CheatId::NoTraffic) RestoreTrafficSettings();
        else if (id == CheatId::Delivery) SyncUnlockAllCitiesFlags();
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

struct SimpleRowDef {
    CollectBit bit;
    app::rows::Kind kind;
    int row_id;
    const char* label;
};

constexpr SimpleRowDef kSimpleRowDefs[] = {
    {CollectBit::Cash5000, app::rows::Kind::Action, kRowCash5000, "+$5,000 Cash"},
    {CollectBit::CashPlus1, app::rows::Kind::Action, kRowCashPlus1, "+$1 Cash"},
    {CollectBit::CashMinus1, app::rows::Kind::Action, kRowCashMinus1, "-$1 Cash"},
    {CollectBit::FullThrottle, app::rows::Kind::Value, kRowFullThrottle, "NOS Auto Refill"},
    {CollectBit::SecretDealer, app::rows::Kind::Value, kRowSecretDealer, "Unlock REMIX Cars"},
    {CollectBit::NoTraffic, app::rows::Kind::Value, kRowNoTraffic, "No Traffic"},
    {CollectBit::ZeroCost, app::rows::Kind::Value, kRowZeroCost, "Free Shop"},
    {CollectBit::Arcade, app::rows::Kind::Value, kRowArcade, "Career Powerups"},
    {CollectBit::Delivery, app::rows::Kind::Value, kRowDelivery, "Unlock All Cities"},
    {CollectBit::Roadtrip, app::rows::Kind::Value, kRowRoadtrip, "Unlock All Cities & Events"},
    {CollectBit::NoDamage, app::rows::Kind::Value, kRowNoDamage, "No Damage"},
    {CollectBit::HyperAgro, app::rows::Kind::Value, kRowHyperAgro, "Hyper Agro Physics"},
};

struct GroupOptionDef {
    CollectBit bit;
    uint32_t hw_val;
    const char* single_label;
    const char* short_name;
};

constexpr GroupOptionDef kSpecialOptions[] = {
    {CollectBit::SpecialZone, 0u, "Force Use Zone", "Zone"},
    {CollectBit::SpecialAgro, 1u, "Force Use Agro", "Agro"},
    {CollectBit::SpecialRoar, 2u, "Force Use Roar", "Roar"},
};

constexpr GroupOptionDef kHeadOptions[] = {
    {CollectBit::HeadFire, kHeadFireHwVal, "Flaming Head", "Flaming"},
    {CollectBit::HeadSmiley, 0u, "Rider Head as Smiley", "Smiley"},
    {CollectBit::HeadPumpkin, 1u, "Rider Head as Pumpkin", "Pumpkin"},
    {CollectBit::HeadBunny, 2u, "Rider Head as Bunny", "Bunny"},
    {CollectBit::HeadSnowman, 3u, "Rider Head as Snowman", "Snowman"},
};

template <size_t N>
void AppendGroupRow(std::vector<app::rows::Row>& out, uint32_t mask, const GroupOptionDef (&opts)[N],
                    int single_row_id, int group_row_id, const char* group_label) {
    size_t collected = 0;
    const GroupOptionDef* first = nullptr;
    for (size_t i = 0; i < N; ++i) {
        if ((mask & BitMask(opts[i].bit)) != 0u) {
            if (!first) first = &opts[i];
            ++collected;
        }
    }
    if (collected == 0u) return;
    if (collected == 1u) {
        out.push_back({app::rows::Kind::Value, single_row_id, first->single_label});
    } else {
        out.push_back({app::rows::Kind::Value, group_row_id, group_label});
    }
}

template <size_t N>
std::string StepGroupValue(uint32_t addr, const GroupOptionDef (&opts)[N], uint32_t mask,
                           bool single_mode, int step, uint32_t fire_flag_addr = 0u) {
    struct Choice {
        uint32_t hw_val;
        const char* label;
    };
    Choice choices[N + 1u]{};
    size_t count = 0;
    choices[count++] = {0xFFFFFFFFu, "Off"};
    for (size_t i = 0; i < N; ++i) {
        if ((mask & BitMask(opts[i].bit)) != 0u) {
            choices[count++] = {opts[i].hw_val, single_mode ? "On" : opts[i].short_name};
        }
    }
    uint32_t cur = safe_mem::Read32(addr, 0xFFFFFFFFu);
    if (fire_flag_addr != 0u && cur == 0xFFFFFFFFu && safe_mem::Read8(fire_flag_addr, 0u) != 0u) {
        cur = kHeadFireHwVal;
    }
    int idx = 0;
    for (size_t i = 0; i < count; ++i) {
        if (choices[i].hw_val == cur) {
            idx = static_cast<int>(i);
            break;
        }
    }
    if (step != 0 && count > 1u) {
        const int n = static_cast<int>(count);
        idx = ((idx + step) % n + n) % n;
        const uint32_t next_val = choices[idx].hw_val;
        if (fire_flag_addr != 0u) {
            if (next_val == kHeadFireHwVal) {
                WriteByteFlag(fire_flag_addr, 1u);
                if (safe_mem::CanAccess(addr, 4u)) kernel::W32(addr, 0xFFFFFFFFu);
            } else {
                WriteByteFlag(fire_flag_addr, 0u);
                if (safe_mem::CanAccess(addr, 4u)) kernel::W32(addr, next_val);
            }
        } else if (safe_mem::CanAccess(addr, 4u)) {
            kernel::W32(addr, next_val);
        }
    }
    return choices[idx].label;
}

const char* StepByteToggle(uint32_t addr, int step) {
    bool on = safe_mem::Read8(addr, 0u) != 0u;
    if (step != 0) {
        on = !on;
        WriteByteFlag(addr, on ? 1u : 0u);
    }
    return on ? "On" : "Off";
}

const char* StepAtomicToggle(std::atomic<bool>& flag, int step) {
    bool on = flag.load(std::memory_order_relaxed);
    if (step != 0) {
        on = !on;
        flag.store(on, std::memory_order_relaxed);
    }
    return on ? "On" : "Off";
}

}  // namespace

bool IsCollectionRow(int row_id) {
    return row_id >= kRowIdBase && row_id < kRowIdEnd;
}

const app::rows::Row* CollectionRows(size_t* count) {
    static std::vector<app::rows::Row> s_rows;
    s_rows.clear();
    const uint32_t mask = EnsureCollectionLoaded();
    for (const auto& def : kSimpleRowDefs) {
        if ((mask & BitMask(def.bit)) != 0u) {
            s_rows.push_back({def.kind, def.row_id, def.label});
        }
    }
    AppendGroupRow(s_rows, mask, kSpecialOptions, kRowSpecialSingle, kRowSpecialGroup, "Special Mode");
    AppendGroupRow(s_rows, mask, kHeadOptions, kRowHeadSingle, kRowHeadGroup, "Rider Head");
    if (s_rows.empty()) {
        s_rows.push_back({app::rows::Kind::Action, kRowEmpty, "No Cheats Collected Yet"});
    }
    if (count) *count = s_rows.size();
    return s_rows.data();
}

std::string FormatCollectionRowValue(int row_id, int step) {
    const uint32_t mask = EnsureCollectionLoaded();
    switch (row_id) {
        case kRowEmpty:
            return "";
        case kRowCash5000:
            if (step != 0) AddCash(5000);
            return "";
        case kRowCashPlus1:
            if (step != 0) {
                const uint32_t prof = GetProfilePtr();
                if (prof && safe_mem::CanAccess(prof + kProfileCashOffset, 4u)) {
                    const uint32_t cur = kernel::R32(prof + kProfileCashOffset);
                    if (cur < 0x186A0u) kernel::W32(prof + kProfileCashOffset, cur + 1u);
                }
            }
            return "";
        case kRowCashMinus1:
            if (step != 0) {
                const uint32_t prof = GetProfilePtr();
                if (prof && safe_mem::CanAccess(prof + kProfileCashOffset, 4u)) {
                    const uint32_t cur = kernel::R32(prof + kProfileCashOffset);
                    if (cur > 0u) kernel::W32(prof + kProfileCashOffset, cur - 1u);
                }
            }
            return "";
        case kRowFullThrottle:
            return StepAtomicToggle(g_state.full_throttle, step);
        case kRowSecretDealer:
            return StepAtomicToggle(g_state.secret_dealer, step);
        case kRowNoTraffic: {
            const char* res = StepAtomicToggle(g_state.no_traffic, step);
            if (step != 0) {
                if (g_state.no_traffic.load(std::memory_order_relaxed)) ApplyTrafficSettings();
                else RestoreTrafficSettings();
            }
            return res;
        }
        case kRowZeroCost:
            return StepAtomicToggle(g_state.zero_cost, step);
        case kRowArcade: {
            const char* res = StepAtomicToggle(g_state.arcade_powerups, step);
            if (step != 0 && g_state.arcade_powerups.load(std::memory_order_relaxed)) {
                ApplyArcadeSettings();
            }
            return res;
        }
        case kRowDelivery: {
            const char* res = StepAtomicToggle(g_state.delivery, step);
            if (step != 0) SyncUnlockAllCitiesFlags();
            return res;
        }
        case kRowRoadtrip: {
            bool on = g_state.native_roadtrip.load(std::memory_order_relaxed) ||
                      (safe_mem::Read8(kUnlockAllCitiesFlag1, 0u) != 0u &&
                       !g_state.delivery.load(std::memory_order_relaxed));
            if (step != 0) {
                on = !on;
                g_state.native_roadtrip.store(on, std::memory_order_relaxed);
                SyncUnlockAllCitiesFlags();
            }
            return on ? "On" : "Off";
        }
        case kRowNoDamage:
            return StepByteToggle(kNativeNoDamageAddr, step);
        case kRowHyperAgro:
            return StepByteToggle(kNativeHyperAgroAddr, step);
        case kRowSpecialSingle:
            return StepGroupValue(kNativeSpecialAddr, kSpecialOptions, mask, true, step);
        case kRowSpecialGroup:
            return StepGroupValue(kNativeSpecialAddr, kSpecialOptions, mask, false, step);
        case kRowHeadSingle:
            return StepGroupValue(kNativeHeadAddr, kHeadOptions, mask, true, step, kNativeFireHeadAddr);
        case kRowHeadGroup:
            return StepGroupValue(kNativeHeadAddr, kHeadOptions, mask, false, step, kNativeFireHeadAddr);
        default:
            return "";
    }
}

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
    bool race_intro = false;
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

        // Exact MC3D race start intro & 3-2-1 countdown detection verified from sub_002C4A10 & sub_001C20E0:
        // race_timer = [0x00557EEC]; [race_timer + 0x04]:
        //   - 2 = pre-race cinematic camera preview (skippable via B)
        //   - 4 = 3-2-1 staging countdown (flips to 5 on the exact GO! frame)
        const uint32_t race_timer = safe_mem::Read32(0x00557EECu, 0u);
        const uint32_t race_state = race_timer ? safe_mem::Read32(race_timer + 0x04u, 0u) : 0u;
        race_intro = (race_timer != 0u && race_state == 0x02u);
        if (race_timer != 0u && race_state == 0x04u &&
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
    tel.race_intro.store(race_intro, std::memory_order_relaxed);
    tel.race_countdown.store(race_countdown, std::memory_order_relaxed);
    static uint64_t s_gameplay_enter_ms = 0u;
    if (ctx != UiContext::Gameplay || race_intro || race_countdown) {
        s_gameplay_enter_ms = 0u;
        g_flash_challenge_ms.store(0u, std::memory_order_relaxed);
    } else if (s_gameplay_enter_ms == 0u) {
        s_gameplay_enter_ms = now_ms;
    }
    const bool cruise_settled = (s_gameplay_enter_ms != 0u) && (now_ms >= s_gameplay_enter_ms + 2500u);
    const uint64_t last_flash_ms = g_flash_challenge_ms.load(std::memory_order_relaxed);
    const bool flash_challenge_ready = cruise_settled &&
                                       (last_flash_ms != 0u) && (now_ms >= last_flash_ms) &&
                                       ((now_ms - last_flash_ms) < 250u);
    tel.flash_challenge_ready.store(flash_challenge_ready, std::memory_order_relaxed);

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
    std::string input;
    if (input_va) {
        input = game::safe_mem::ReadSanitizedCString(input_va, 64);
        if (!input.empty() && game::cheats::TryHandleCheatCode(input)) {
            ctx.eax = 0xFFFFFFFFu;
            ctx.esp += 8u;
            return;
        }
    }
    __imp__sub_001923B0(ctx, base);
    if (ctx.eax == 0xFFFFFFFFu && !input.empty()) {
        game::cheats::CollectBit bit{};
        if (game::cheats::ParseNativeCheatCode(input, &bit)) {
            game::cheats::RecordCollectedBit(bit);
            if (bit == game::cheats::CollectBit::Roadtrip) {
                game::cheats::g_state.native_roadtrip.store(true, std::memory_order_relaxed);
            } else if (bit == game::cheats::CollectBit::HeadFire) {
                if (game::safe_mem::CanAccess(game::cheats::kNativeHeadAddr, 4u)) {
                    kernel::W32(game::cheats::kNativeHeadAddr, 0xFFFFFFFFu);
                }
            } else if (bit == game::cheats::CollectBit::HeadSmiley ||
                       bit == game::cheats::CollectBit::HeadPumpkin ||
                       bit == game::cheats::CollectBit::HeadBunny ||
                       bit == game::cheats::CollectBit::HeadSnowman) {
                game::cheats::WriteByteFlag(game::cheats::kNativeFireHeadAddr, 0u);
            }
        }
    }
}

// UI button-to-event lookup: __stdcall uint32_t sub_00206400(uint32_t btn_idx) [ret 0x04].
// Button indices: 1=B (0xC0000BB9), 3=Back (0xC0000BCD), 7=DPadRight (0xC0000BBD), 9=X (0xC0000BEB).
// While typing in uiKeyboard (Cheat Codes or Name Entry), pressing Back/B deletes text via X
// (0xC0000BEB) until empty, and only triggers Back after release + brief cooldown on an empty field.
X86_FUNC(sub_00206400) {
    const uint32_t btn_idx = game::safe_mem::Read32(ctx.esp + 4u, 0xFFFFFFFFu);
    if (btn_idx == 1u || btn_idx == 3u) {
        const size_t slot = (btn_idx == 1u) ? 0u : 1u;
        bool has_text = false;
        if (game::cheats::FindActiveKeyboardVa(&has_text) != 0u) {
            const uint64_t now_ms = game::cheats::SteadyNowMs();
            if (has_text) {
                game::cheats::g_back_delete_hold[slot].store(true, std::memory_order_relaxed);
                game::cheats::g_back_delete_cooldown_ms.store(now_ms + 350ull, std::memory_order_relaxed);
                game::cheats::t_back_forced_backspace = true;
                ctx.eax = 0xC0000BEBu;
                ctx.esp += 8u;
                return;
            }
            if (game::cheats::g_back_delete_hold[slot].load(std::memory_order_relaxed) ||
                now_ms < game::cheats::g_back_delete_cooldown_ms.load(std::memory_order_relaxed)) {
                ctx.eax = 0u;
                ctx.esp += 8u;
                return;
            }
        }
    } else if (btn_idx == 9u) {
        game::cheats::t_back_forced_backspace = false;
    }
    __imp__sub_00206400(ctx, base);
}

// UI input poller: __thiscall void sub_002071C0() [ret 0].
// Clears the Back-delete hold latch only once an active uiInput sees B (0xC5) or Back (0xC7) released.
X86_FUNC(sub_002071C0) {
    const uint32_t ui_input_va = ctx.ecx;
    __imp__sub_002071C0(ctx, base);
    game::cheats::t_back_forced_backspace = false;
    if (game::cheats::FindActiveKeyboardVa(nullptr) == 0u) {
        game::cheats::g_back_delete_hold[0].store(false, std::memory_order_relaxed);
        game::cheats::g_back_delete_hold[1].store(false, std::memory_order_relaxed);
        game::cheats::g_back_delete_cooldown_ms.store(0ull, std::memory_order_relaxed);
    } else if (ui_input_va != 0u && game::safe_mem::CanAccess(ui_input_va + 0xD0u, 1u) &&
               (game::safe_mem::Read8(ui_input_va + 0x44u, 0u) & 0x01u) != 0u) {
        if (game::safe_mem::Read8(ui_input_va + 0xC4u + 1u, 0u) == 0u) {
            game::cheats::g_back_delete_hold[0].store(false, std::memory_order_relaxed);
        }
        if (game::safe_mem::Read8(ui_input_va + 0xC4u + 3u, 0u) == 0u) {
            game::cheats::g_back_delete_hold[1].store(false, std::memory_order_relaxed);
        }
    }
}

// uiKeyboard::OnMessage: __thiscall void sub_00350120(arg0, msgId, arg2) [ret 0x0C].
// Ensure Back->X backspace still runs even if cursor was moved onto the side action column ([kbd+0x4D]!=0).
X86_FUNC(sub_00350120) {
    const uint32_t kbd = ctx.ecx;
    const uint32_t msg_id = game::safe_mem::Read32(ctx.esp + 8u, 0u);
    if (game::cheats::t_back_forced_backspace && msg_id == 0xC0000BEBu &&
        kbd != 0u && game::safe_mem::CanAccess(kbd + 0x5Cu, 1u)) {
        const uint8_t side_before = X86_MEM8(kbd + 0x4Du);
        const uint8_t key_before = X86_MEM8(kbd + 0x5Bu);
        if (side_before != 0u) {
            X86_MEM8(kbd + 0x4Du) = 0u;
            __imp__sub_00350120(ctx, base);
            if (X86_MEM8(kbd + 0x5Bu) == key_before) {
                X86_MEM8(kbd + 0x4Du) = side_before;
            }
            return;
        }
    }
    __imp__sub_00350120(ctx, base);
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

// Flash swfINSTANCE::Draw: __thiscall void sub_000659D0(swfDRAWSTATE* state) [ret 0x04].
// Skips drawing ALL Xbox controller button icons & prompt clips (A/B/X/Y/L/R/triggers/RStick/prompt*)
// across Main Menu, Save/Load, Options, Pause, Garage, Vinyl, and Full City Map while touchscreen is active.
// Restores immediately on the next frame whenever an external controller button is pressed.
X86_FUNC(sub_000659D0) {
    if (input::TouchActive() && ctx.ecx != 0u && game::safe_mem::CanAccess(ctx.ecx + 0x7Cu, 4u)) {
        const uint32_t name_va = kernel::R32(ctx.ecx + 0x7Cu);
        if (name_va != 0u && game::cheats::IsControllerPromptClip(name_va)) {
            ctx.esp += 8u;  // __thiscall ret 0x04
            return;
        }
    }
    __imp__sub_000659D0(ctx, base);
}

// Track Lobby/UI CMenuTable instance named "ButtonPromptsTable" (0x0045CCF8).
X86_FUNC(sub_00209000) {
    if (ctx.ecx != 0u && game::safe_mem::CanAccess(ctx.esp + 4u, 4u)) {
        const uint32_t name_va = X86_MEM32(ctx.esp + 4u);
        if (name_va == 0x0045CCF8u) {
            game::cheats::g_button_prompts_table_va.store(ctx.ecx, std::memory_order_relaxed);
        }
    }
    __imp__sub_00209000(ctx, base);
}

// Hide non-Flash ButtonPromptsTable widget on touchscreen; restore on external controller press.
// Signature: __thiscall void sub_002090A0() [ret 0].
X86_FUNC(sub_002090A0) {
    if (input::TouchActive() && ctx.ecx != 0u &&
        ctx.ecx == game::cheats::g_button_prompts_table_va.load(std::memory_order_relaxed)) {
        ctx.esp += 4u;  // __thiscall ret 0
        return;
    }
    __imp__sub_002090A0(ctx, base);
}

// Hide pre-race cinematic camera preview "Press B to skip" / "Pause camera" HUD prompt on touchscreen.
// Signature: __thiscall void sub_002C4A10() [ret 0].
X86_FUNC(sub_002C4A10) {
    if (input::TouchActive()) {
        ctx.esp += 4u;  // __thiscall ret 0
        return;
    }
    __imp__sub_002C4A10(ctx, base);
}

namespace {
thread_local bool t_flash_box_candidate = false;
}  // namespace

// Detect when MC3D is actively rendering the 3D world "flash to begin" mini-box (via sub_001DB557 after
// clipping & alpha checks pass) directly in front of the player car's headlights for either an eligible
// roaming racer (0x1D7ECC) or an optional Free Cruise city/club race currently in activation range (0x1D821B).
X86_FUNC(sub_001DB557) {
    if (t_flash_box_candidate) {
        t_flash_box_candidate = false;
        game::cheats::g_flash_challenge_ms.store(game::cheats::SteadyNowMs(), std::memory_order_relaxed);
    }
    __imp__sub_001DB557(ctx, base);
}

X86_FUNC(sub_001DB0D0) {
    const uint32_t entry_esp = ctx.esp;
    const uint32_t ret_va = game::safe_mem::Read32(entry_esp, 0u);
    const bool is_roaming_racer = (ret_va == 0x1D7ECCu);
    const bool is_optional_race = (ret_va == 0x1D821Bu);

    auto read_f32 = [](uint32_t va, uint32_t offset) -> float {
        const uint32_t bits = game::safe_mem::Read32(va, offset);
        float out = 0.0f;
        std::memcpy(&out, &bits, sizeof(out));
        return out;
    };

    bool eligible = false;
    if ((is_roaming_racer || is_optional_race) &&
        game::safe_mem::Read32(game::cheats::kGameStateAddr, 0u) == 2u &&
        game::safe_mem::Read8(0x005341A0u, 0u) != 0u) {
        const uint32_t hookmen_wrap = game::safe_mem::Read32(0x00533E24u, 0u);
        const uint32_t hookmen = hookmen_wrap ? game::safe_mem::Read32(hookmen_wrap + 0x04u, 0u) : 0u;
        const uint32_t race_timer = game::safe_mem::Read32(0x00557EECu, 0u);
        const uint32_t race_state = race_timer ? game::safe_mem::Read32(race_timer + 0x04u, 0u) : 0u;
        const uint32_t car_mgr = game::safe_mem::Read32(0x00558814u, 0u);
        const uint32_t player_car = car_mgr ? game::safe_mem::Read32(car_mgr + 0x08u, 0u) : 0u;
        const uint32_t veh_p14 = player_car ? game::safe_mem::Read32(player_car + 0x14u, 0u) : 0u;
        const uint32_t veh_phys = veh_p14 ? game::safe_mem::Read32(veh_p14 + 0x08u, 0u) : 0u;
        const uint32_t veh_mat = veh_p14 ? game::safe_mem::Read32(veh_p14 + 0x1Cu, 0u) : 0u;

        bool circle_mode = false;
        const uint32_t veh_p04 = veh_p14 ? game::safe_mem::Read32(veh_p14 + 0x04u, 0u) : 0u;
        const uint32_t cfg_holder = veh_p04 ? game::safe_mem::Read32(veh_p04 + 0x64u, 0u) : 0u;
        const uint32_t cfg_idx = cfg_holder ? game::safe_mem::Read32(cfg_holder + 0x04u, 0u) : 0xFFFFFFFFu;
        if (cfg_idx < 16u) {
            const uint32_t cfg_entry = 0x00558848u + cfg_idx * 0x140u;
            circle_mode = (game::safe_mem::Read32(cfg_entry + 0x80u, 0u) == 6u) &&
                          (game::safe_mem::Read32(cfg_entry + 0x84u, 0u) == 1u);
        }

        const bool not_in_race = (veh_phys != 0u) &&
                                 ((game::safe_mem::Read8(veh_phys + 0x15Cu, 0u) & 0x02u) == 0u) &&
                                 (race_state != 2u) && (race_state != 4u) && (race_state < 7u);
        const bool hookmen_ready = (hookmen != 0u) && (game::safe_mem::Read8(hookmen + 0x0Du, 0u) == 0u);
        const int32_t opt_idx = hookmen ? static_cast<int32_t>(game::safe_mem::Read32(hookmen + 0x125Cu, 0xFFFFFFFFu)) : -1;

        if (!circle_mode && not_in_race && hookmen_ready && veh_mat != 0u &&
            (!is_optional_race || opt_idx >= 0)) {
            const uint32_t target_pos_va = game::safe_mem::Read32(entry_esp, 0x08u);
            const uint32_t player_pos_va = game::safe_mem::Read32(entry_esp, 0x0Cu);
            if (target_pos_va != 0u && player_pos_va != 0u) {
                const float tx = read_f32(target_pos_va, 0x00u);
                const float ty = read_f32(target_pos_va, 0x04u);
                const float tz = read_f32(target_pos_va, 0x08u);
                const float px = read_f32(player_pos_va, 0x00u);
                const float py = read_f32(player_pos_va, 0x04u);
                const float pz = read_f32(player_pos_va, 0x08u);
                const float fx = -read_f32(veh_mat, 0x28u);
                const float fz = -read_f32(veh_mat, 0x30u);
                const float dx = tx - px;
                const float dy = ty - py;
                const float dz = tz - pz;
                const float dist_xz_sq = dx * dx + dz * dz;
                const float max_r = is_roaming_racer ? 22.0f : 20.0f;
                const float fwd_len = std::hypot(fx, fz);
                if (std::isfinite(dx) && std::isfinite(dy) && std::isfinite(dz) &&
                    std::isfinite(fx) && std::isfinite(fz) && fwd_len > 0.01f &&
                    std::fabs(dy) <= 5.0f && dist_xz_sq >= 2.25f && dist_xz_sq <= max_r * max_r) {
                    const float cos_angle = (fx * dx + fz * dz) / (fwd_len * std::sqrt(dist_xz_sq));
                    eligible = (cos_angle >= 0.50f);
                }
            }
        }
    }

    t_flash_box_candidate = eligible;
    __imp__sub_001DB0D0(ctx, base);
    t_flash_box_candidate = false;
}
