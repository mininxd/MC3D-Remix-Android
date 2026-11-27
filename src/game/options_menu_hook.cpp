#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "common/menu.h"
#include "core/log.h"
#include "cpu/x86_context.h"
#include "cpu/x86_ops.h"
#include "game/cheats_hook.h"
#include "game/guest_mem_safe.h"
#include "kernel/memory.h"

extern "C" void __imp__sub_00154E40(X86Context& ctx, uint8_t* base);
extern "C" void __imp__sub_0018C770(X86Context& ctx, uint8_t* base);
extern "C" void __imp__sub_0018CAD0(X86Context& ctx, uint8_t* base);
extern "C" void __imp__sub_0018D610(X86Context& ctx, uint8_t* base);
extern "C" void __imp__sub_00163830(X86Context& ctx, uint8_t* base);
extern "C" void __imp__sub_00163C40(X86Context& ctx, uint8_t* base);
extern "C" void __imp__sub_0018DD50(X86Context& ctx, uint8_t* base);
extern "C" void __imp__sub_0018DB10(X86Context& ctx, uint8_t* base);
extern "C" void __imp__sub_00348BE0(X86Context& ctx, uint8_t* base);
extern "C" void __imp__sub_00348D70(X86Context& ctx, uint8_t* base);

extern "C" void sub_001395E0(X86Context& ctx, uint8_t* base);
extern "C" void sub_00348950(X86Context& ctx, uint8_t* base);

namespace {

constexpr uint32_t kMenuMgrGlobalPtr = 0x0050F148u;
constexpr uint32_t kFlashRootGlobalPtr = 0x0050F15Cu;
constexpr uint32_t kFlashVarTitle = 0x0045B9F8u;        // "title"
constexpr uint32_t kFlashVarSectionName = 0x0045772Cu;  // "sectionName"
constexpr uint32_t kActionCheatCodes = 0x32u;
constexpr uint32_t kActionGameOptions = 0x41u;
constexpr uint32_t kActionDisplayOptions = 0x7F4Bu;
constexpr uint32_t kActionQualityOptions = 0x7F4Cu;
constexpr uint32_t kActionCheatOptions = 0x7F4Du;
constexpr uint32_t kFnMenuAction = 0x00163830u;
constexpr uint32_t kFnMainMenuNetworking = 0x0018C5A0u;
constexpr uint32_t kFnSettingCallback = 0x0018DB10u;
constexpr uint32_t kMaxListItems = 16u;
constexpr uint32_t kMaxRootListItems = 20u;
constexpr uint32_t kScratchBytes = 192u;
constexpr uint32_t kListStructMinBytes = 0x270u;

enum class NativePage : int {
    None = 0,
    Display = 1,
    Quality = 2,
    Cheats = 3,
};

std::atomic<int> g_native_page{static_cast<int>(NativePage::None)};

thread_local bool g_in_main_menu_populate = false;
thread_local bool g_skipped_networking = false;

thread_local bool g_in_options_populate = false;
thread_local bool g_inserted_cheat_options = false;
thread_local uint32_t g_cheat_codes_handle = 0u;
thread_local uint32_t g_cheat_codes_list_va = 0u;
thread_local uint32_t g_cheat_codes_owner_va = 0u;

struct ScopedMainMenuPopulate {
    ScopedMainMenuPopulate() {
        g_in_main_menu_populate = true;
        g_skipped_networking = false;
    }
    ~ScopedMainMenuPopulate() {
        g_in_main_menu_populate = false;
        g_skipped_networking = false;
    }
};

struct ScopedOptionsMenuPopulate {
    ScopedOptionsMenuPopulate() {
        g_in_options_populate = true;
        g_inserted_cheat_options = false;
        g_cheat_codes_handle = 0u;
        g_cheat_codes_list_va = 0u;
        g_cheat_codes_owner_va = 0u;
    }
    ~ScopedOptionsMenuPopulate() {
        g_in_options_populate = false;
        g_cheat_codes_handle = 0u;
        g_cheat_codes_list_va = 0u;
        g_cheat_codes_owner_va = 0u;
    }
};

struct NativeEntry {
    app::rows::Row row{};
    bool is_cheat = false;
    game::cheats::CheatId cheat_id = game::cheats::CheatId::Cash5000;
    uint32_t handle = 0u;
};

NativeEntry g_entries[kMaxListItems]{};
uint32_t g_entry_count = 0u;

struct SavedRegs {
    uint32_t esp, eax, ebx, ecx, edx, esi, edi, ebp;

    static SavedRegs Save(const X86Context& ctx) {
        return {ctx.esp, ctx.eax, ctx.ebx, ctx.ecx, ctx.edx, ctx.esi, ctx.edi, ctx.ebp};
    }

    void Restore(X86Context& ctx) const {
        ctx.esp = esp;
        ctx.eax = eax;
        ctx.ebx = ebx;
        ctx.ecx = ecx;
        ctx.edx = edx;
        ctx.esi = esi;
        ctx.edi = edi;
        ctx.ebp = ebp;
    }
};

void WriteGuestUtf16Ascii(uint32_t guest_va, const char* text, size_t max_chars) {
    auto* dst = static_cast<uint16_t*>(kernel::HostPtr(guest_va));
    size_t out = 0;
    const auto* src = reinterpret_cast<const unsigned char*>(text);
    for (size_t i = 0; src[i] != 0 && out + 1 < max_chars;) {
        uint32_t cp = src[i];
        if ((cp & 0x80u) == 0) {
            i += 1;
        } else if ((cp & 0xE0u) == 0xC0u && src[i + 1]) {
            cp = ((cp & 0x1Fu) << 6) | (src[i + 1] & 0x3Fu);
            i += 2;
        } else if ((cp & 0xF0u) == 0xE0u && src[i + 1] && src[i + 2]) {
            cp = ((cp & 0x0Fu) << 12) | ((src[i + 1] & 0x3Fu) << 6) | (src[i + 2] & 0x3Fu);
            i += 3;
        } else {
            cp = '?';
            i += 1;
        }
        dst[out++] = static_cast<uint16_t>(cp <= 0xFFFFu ? cp : '?');
    }
    dst[out] = 0u;
}

uint32_t GetOptionScreenList(uint32_t* out_owner = nullptr) {
    const uint32_t mgr_va = game::safe_mem::Read32(kMenuMgrGlobalPtr, 0u);
    const uint32_t menu_obj_va = mgr_va ? game::safe_mem::Read32(mgr_va + 0x0Cu, 0u) : 0u;
    const uint32_t list_va = menu_obj_va ? game::safe_mem::Read32(menu_obj_va + 0xFCu, 0u) : 0u;
    const uint32_t owner_va = menu_obj_va ? game::safe_mem::Read32(menu_obj_va + 0x0Cu, 0u) : 0u;
    if (out_owner) *out_owner = owner_va;
    if (!list_va || !game::safe_mem::CanAccess(list_va, kListStructMinBytes)) return 0u;
    return list_va;
}

std::string FormatEntryLabel(const NativeEntry& e) {
    if (e.is_cheat) {
        if (e.cheat_id == game::cheats::CheatId::Cash5000) {
            const int32_t cash = game::cheats::GetCurrentCash();
            char buf[64];
            if (cash >= 0) {
                std::snprintf(buf, sizeof(buf), "%s: $%d", game::cheats::GetCheatMenuLabel(e.cheat_id), cash);
            } else {
                std::snprintf(buf, sizeof(buf), "%s: +$5000", game::cheats::GetCheatMenuLabel(e.cheat_id));
            }
            return buf;
        }
        return std::string(game::cheats::GetCheatMenuLabel(e.cheat_id)) + ": " +
               (game::cheats::IsCheatActive(e.cheat_id) ? "On" : "Off");
    }
    return std::string(e.row.label) + ": " + app::rows::Value(e.row, 0);
}

void UpdateListRowText(X86Context& ctx, uint8_t* base, uint32_t list_va, uint32_t handle, const std::string& text) {
    if (!handle || !game::safe_mem::CanAccess(handle, 16u)) return;
    if (ctx.esp <= kScratchBytes + 64u) return;

    const SavedRegs saved = SavedRegs::Save(ctx);
    const uint32_t scratch_va = saved.esp - kScratchBytes;
    if (!game::safe_mem::CanAccess(scratch_va, kScratchBytes)) return;

    ctx.esp = scratch_va;
    WriteGuestUtf16Ascii(scratch_va, text.c_str(), kScratchBytes / 2u);
    X86_PUSH32(scratch_va);
    X86_PUSH32(2u);
    X86_PUSH32(handle);
    ctx.ecx = list_va;
    X86_PUSH32(0u);
    __imp__sub_00348D70(ctx, base);

    saved.Restore(ctx);
}

void CallWidgetSetVisible(X86Context& ctx, uint8_t* base, uint32_t widget_va, uint32_t visible) {
    if (!widget_va || !game::safe_mem::CanAccess(widget_va, 4u)) return;
    const uint32_t vtbl_va = game::safe_mem::Read32(widget_va, 0u);
    if (!vtbl_va || !game::safe_mem::CanAccess(vtbl_va + 0xB0u, 4u)) return;
    const uint32_t fn_va = game::safe_mem::Read32(vtbl_va + 0xB0u, 0u);
    if (!fn_va) return;

    const SavedRegs saved = SavedRegs::Save(ctx);
    X86_PUSH32(visible);
    ctx.ecx = widget_va;
    X86_PUSH32(0u);
    x86_icall(ctx, base, fn_va);
    saved.Restore(ctx);
}

void AppendRootOptionItem(X86Context& ctx, uint8_t* base, uint32_t list_va, uint32_t owner_va,
                          uint32_t action_id, const char* label) {
    const uint32_t active_list = game::safe_mem::Read32(list_va + 0x258u, 0xFFFFFFFFu);
    if (active_list >= 4u) return;

    const uint32_t item_idx = game::safe_mem::Read32(list_va + active_list * 4u + 0x238u, kMaxRootListItems);
    if (item_idx >= kMaxRootListItems || ctx.esp <= kScratchBytes + 64u) return;

    const SavedRegs saved = SavedRegs::Save(ctx);
    const uint32_t scratch_va = saved.esp - kScratchBytes;
    if (!game::safe_mem::CanAccess(scratch_va, kScratchBytes)) return;

    ctx.esp = scratch_va;
    WriteGuestUtf16Ascii(scratch_va, label, kScratchBytes / 2u);

    // mcCallback layout (matches sub_0020ED00 with flag=0): { obj, fn, stored_param, cb_type=2 }
    X86_PUSH32(2u);
    X86_PUSH32(action_id);
    X86_PUSH32(kFnMenuAction);
    X86_PUSH32(owner_va);
    X86_PUSH32(0u);
    X86_PUSH32(0u);
    X86_PUSH32(item_idx);
    ctx.ecx = list_va;
    X86_PUSH32(0u);
    __imp__sub_00348BE0(ctx, base);

    const uint32_t item_handle = ctx.eax;
    if (item_handle != 0u && game::safe_mem::CanAccess(item_handle, 16u)) {
        X86_PUSH32(scratch_va);
        X86_PUSH32(2u);
        X86_PUSH32(item_handle);
        ctx.ecx = list_va;
        X86_PUSH32(0u);
        __imp__sub_00348D70(ctx, base);
    }

    saved.Restore(ctx);
}

void AppendCustomOptionsItems(X86Context& ctx, uint8_t* base) {
    uint32_t owner_va = 0u;
    const uint32_t list_va = GetOptionScreenList(&owner_va);
    if (!list_va || !owner_va || !game::safe_mem::CanAccess(owner_va, 4u)) return;

    AppendRootOptionItem(ctx, base, list_va, owner_va, kActionDisplayOptions, "Display Options");
    AppendRootOptionItem(ctx, base, list_va, owner_va, kActionQualityOptions, "Quality Options");
    if (!g_inserted_cheat_options && game::cheats::HasAnyUnlockedCheat()) {
        AppendRootOptionItem(ctx, base, list_va, owner_va, kActionCheatOptions, "Cheat Options");
    }
}

void PopulateNativeOptionsList(X86Context& ctx, uint8_t* base, uint32_t screen_va, NativePage page) {
    uint32_t owner_va = 0u;
    const uint32_t list_va = GetOptionScreenList(&owner_va);
    if (!list_va || !screen_va || ctx.esp <= kScratchBytes + 128u) return;

    // Match __imp__sub_0018DD50 preamble: ensure list widget is visible and clear sentinel flag.
    CallWidgetSetVisible(ctx, base, list_va, 1u);
    if (owner_va && game::safe_mem::CanAccess(owner_va + 0x224u, 1u) &&
        X86_MEM8(owner_va + 0x224u) == 0xFFu) {
        X86_MEM8(owner_va + 0x224u) = 0u;
    }

    const SavedRegs saved = SavedRegs::Save(ctx);

    // Reset CMenuList rows for column 0 via CMenuList::Reset(1) without touching list layout/padding.
    ctx.ecx = list_va;
    X86_PUSH32(1u);
    X86_PUSH32(0u);
    sub_00348950(ctx, base);

    const uint32_t list_cap = std::min<uint32_t>(
        kMaxListItems, std::max<uint32_t>(1u, game::safe_mem::Read32(list_va + 0x230u, kMaxListItems)));

    g_entry_count = 0u;
    if (page == NativePage::Cheats) {
        for (int i = 0; i < static_cast<int>(game::cheats::CheatId::Count) && g_entry_count < list_cap; ++i) {
            const auto cid = static_cast<game::cheats::CheatId>(i);
            if (!game::cheats::IsCheatUnlocked(cid)) continue;
            NativeEntry entry{};
            entry.is_cheat = true;
            entry.cheat_id = cid;
            entry.row.kind = (cid == game::cheats::CheatId::Cash5000) ? app::rows::Kind::Action : app::rows::Kind::Value;
            entry.row.label = game::cheats::GetCheatMenuLabel(cid);
            g_entries[g_entry_count++] = entry;
        }
    } else {
        const int app_page = (page == NativePage::Quality) ? app::rows::kQualityPage : app::rows::kDisplayPage;
        size_t total = 0;
        const app::rows::Row* rows = app::rows::Page(app_page, &total);
        for (size_t i = 0; i < total && g_entry_count < list_cap; ++i) {
            if (rows[i].kind == app::rows::Kind::Value || rows[i].kind == app::rows::Kind::Action) {
                NativeEntry entry{};
                entry.row = rows[i];
                g_entries[g_entry_count++] = entry;
            }
        }
    }

    const uint32_t scratch_va = saved.esp - kScratchBytes;
    if (game::safe_mem::CanAccess(scratch_va, kScratchBytes)) {
        for (uint32_t r = 0; r < g_entry_count; ++r) {
            NativeEntry& e = g_entries[r];
            // row_type=0 triggers callback on A-button click; row_type=2 enables Left (-1) / Right (+1).
            const uint32_t row_type = (e.row.kind == app::rows::Kind::Action) ? 0u : 2u;
            ctx.esp = scratch_va;
            X86_PUSH32(2u);
            X86_PUSH32(1u);
            X86_PUSH32(kFnSettingCallback);
            X86_PUSH32(screen_va);
            X86_PUSH32(row_type);
            X86_PUSH32(0u);
            X86_PUSH32(r);
            ctx.ecx = list_va;
            X86_PUSH32(0u);
            __imp__sub_00348BE0(ctx, base);

            e.handle = ctx.eax;
            if (e.handle != 0u && game::safe_mem::CanAccess(e.handle, 16u)) {
                ctx.esp = scratch_va;
                const std::string text = FormatEntryLabel(e);
                WriteGuestUtf16Ascii(scratch_va, text.c_str(), kScratchBytes / 2u);
                X86_PUSH32(scratch_va);
                X86_PUSH32(2u);
                X86_PUSH32(e.handle);
                ctx.ecx = list_va;
                X86_PUSH32(0u);
                __imp__sub_00348D70(ctx, base);
            }
        }
    }

    saved.Restore(ctx);
}

void RefreshVisibleRows(X86Context& ctx, uint8_t* base) {
    const uint32_t list_va = GetOptionScreenList();
    if (!list_va) return;
    for (uint32_t i = 0; i < g_entry_count; ++i) {
        if (g_entries[i].handle) {
            UpdateListRowText(ctx, base, list_va, g_entries[i].handle, FormatEntryLabel(g_entries[i]));
        }
    }
    if (game::safe_mem::CanAccess(list_va + 0x331u, 1u)) {
        X86_MEM8(list_va + 0x331u) = 1u;
    }
}

void ApplyNativeScreenTitle(X86Context& ctx, uint8_t* base, NativePage page) {
    if (page == NativePage::None || ctx.esp <= 64u) return;

    const uint32_t mgr_va = game::safe_mem::Read32(kMenuMgrGlobalPtr, 0u);
    const uint32_t cur_menu_va = mgr_va ? game::safe_mem::Read32(mgr_va + 0x0Cu, 0u) : 0u;
    const uint32_t owner_va = cur_menu_va ? game::safe_mem::Read32(cur_menu_va + 0x0Cu, 0u) : 0u;
    if (!owner_va) return;

    const uint32_t title_buf_va = owner_va + 0x60u;
    if (!game::safe_mem::CanAccess(title_buf_va, 0x78u)) return;

    const uint32_t flash_root_va = game::safe_mem::Read32(kFlashRootGlobalPtr, 0u);
    // In Main Menu (bg_options.xbck), the narrow uppercase title slot wraps two-word titles.
    // Use single-word "Display" / "Quality" / "Cheats" in Main Menu and full titles in Pause Menu.
    const char* title = nullptr;
    if (flash_root_va) {
        if (page == NativePage::Display) title = "Display";
        else if (page == NativePage::Quality) title = "Quality";
        else title = "Cheats";
    } else {
        if (page == NativePage::Display) title = "Display Options";
        else if (page == NativePage::Quality) title = "Quality Options";
        else title = "Cheat Options";
    }

    auto* dst = static_cast<char*>(kernel::HostPtr(title_buf_va));
    const size_t len = std::min<size_t>(std::strlen(title), 0x77u);
    std::memcpy(dst, title, len);
    dst[len] = '\0';

    uint32_t target_flash_va = 0u;
    uint32_t var_name_va = 0u;
    if (flash_root_va) {
        target_flash_va = game::safe_mem::Read32(flash_root_va + 0xFACu, 0u);
        var_name_va = kFlashVarTitle;
    } else {
        target_flash_va = game::safe_mem::Read32(owner_va + 0x1E8u, 0u);
        var_name_va = kFlashVarSectionName;
    }
    if (!target_flash_va || !game::safe_mem::CanAccess(target_flash_va, 0x10u)) return;

    const SavedRegs saved = SavedRegs::Save(ctx);
    X86_PUSH32(title_buf_va);
    X86_PUSH32(var_name_va);
    ctx.ecx = target_flash_va;
    X86_PUSH32(0u);
    sub_001395E0(ctx, base);
    saved.Restore(ctx);
}

}  // namespace

// Override screen header title setter so custom Options screens never flash "GAME OPTIONS".
extern "C" void sub_00154E40(X86Context& ctx, uint8_t* base) {
    const auto page = static_cast<NativePage>(g_native_page.load());
    if (page != NativePage::None) {
        ApplyNativeScreenTitle(ctx, base, page);
        ctx.esp += 4u;  // __cdecl ret 0
        return;
    }
    __imp__sub_00154E40(ctx, base);
}

// Hide Networking item from Main Menu list while keeping remaining items contiguously indexed.
extern "C" void sub_0018C770(X86Context& ctx, uint8_t* base) {
    ScopedMainMenuPopulate guard;
    __imp__sub_0018C770(ctx, base);
}

// Intercept CMenuList::AddItem (sub_00348BE0: __thiscall ret 0x1C):
//  - Main Menu: filter Networking row and keep indices contiguous.
//  - Options Menu: detect CM_Cheat_Codes (0x32) so Cheat Options is inserted directly underneath it.
extern "C" void sub_00348BE0(X86Context& ctx, uint8_t* base) {
    if (g_in_main_menu_populate) {
        const uint32_t cb_fn = game::safe_mem::Read32(ctx.esp + 20u, 0u);
        if (cb_fn == kFnMainMenuNetworking) {
            g_skipped_networking = true;
            ctx.eax = 0u;
            ctx.esp += 32u;  // return address + 0x1C args
            return;
        }
        if (g_skipped_networking && game::safe_mem::CanAccess(ctx.esp + 4u, 4u)) {
            const uint32_t idx = X86_MEM32(ctx.esp + 4u);
            if (idx > 0u) {
                X86_MEM32(ctx.esp + 4u) = idx - 1u;
            }
        }
    } else if (g_in_options_populate) {
        if (g_inserted_cheat_options && game::safe_mem::CanAccess(ctx.esp + 4u, 4u)) {
            X86_MEM32(ctx.esp + 4u) = X86_MEM32(ctx.esp + 4u) + 1u;
        }
        const uint32_t cb_obj = game::safe_mem::Read32(ctx.esp + 16u, 0u);
        const uint32_t cb_fn = game::safe_mem::Read32(ctx.esp + 20u, 0u);
        const uint32_t stored_param = game::safe_mem::Read32(ctx.esp + 24u, 0u);
        const uint32_t list_va = ctx.ecx;
        __imp__sub_00348BE0(ctx, base);
        if (cb_fn == kFnMenuAction && stored_param == kActionCheatCodes && ctx.eax != 0u) {
            g_cheat_codes_handle = ctx.eax;
            g_cheat_codes_list_va = list_va;
            g_cheat_codes_owner_va = cb_obj;
        }
        return;
    }
    __imp__sub_00348BE0(ctx, base);
}

// Intercept CMenuList::SetItemText (sub_00348D70: __thiscall ret 0x0C):
//  - Main Menu: skip label assignment for filtered Networking handle.
//  - Options Menu: immediately insert "Cheat Options" directly after "Cheat Codes" when unlocked.
extern "C" void sub_00348D70(X86Context& ctx, uint8_t* base) {
    const uint32_t handle = game::safe_mem::Read32(ctx.esp + 4u, 0u);
    if (g_in_main_menu_populate && handle == 0u) {
        ctx.esp += 16u;  // return address + 0x0C args
        return;
    }
    __imp__sub_00348D70(ctx, base);

    if (g_in_options_populate && !g_inserted_cheat_options &&
        g_cheat_codes_handle != 0u && handle == g_cheat_codes_handle &&
        game::cheats::HasAnyUnlockedCheat()) {
        g_cheat_codes_handle = 0u;
        if (g_cheat_codes_list_va && g_cheat_codes_owner_va) {
            AppendRootOptionItem(ctx, base, g_cheat_codes_list_va, g_cheat_codes_owner_va,
                                 kActionCheatOptions, "Cheat Options");
            g_inserted_cheat_options = true;
        }
    }
}

// Hide Main Menu Xbox Live sign-in status widget ([this + 0x1C0], "Not Signed In" / "Signed in...").
extern "C" void sub_0018CAD0(X86Context& ctx, uint8_t* base) {
    const uint32_t status_widget_va = game::safe_mem::Read32(ctx.ecx + 0x1C0u, 0u);
    CallWidgetSetVisible(ctx, base, status_widget_va, 0u);
    ctx.esp += 4u;  // __thiscall ret 0
}

// Override Options menu populate: insert Cheat Options right after Cheat Codes (when unlocked)
// and append Display Options and Quality Options items.
extern "C" void sub_0018D610(X86Context& ctx, uint8_t* base) {
    g_native_page.store(static_cast<int>(NativePage::None));
    bool inserted_inline = false;
    {
        ScopedOptionsMenuPopulate guard;
        __imp__sub_0018D610(ctx, base);
        inserted_inline = g_inserted_cheat_options;
    }
    g_inserted_cheat_options = inserted_inline;
    AppendCustomOptionsItems(ctx, base);
    g_inserted_cheat_options = false;
}

// Route Display Options, Quality Options, and Cheat Options through mcMenuGameOptions host screen (0x41).
extern "C" void sub_00163830(X86Context& ctx, uint8_t* base) {
    const uint32_t action = game::safe_mem::Read32(ctx.esp + 4u, 0u);
    if (action == kActionDisplayOptions) {
        g_native_page.store(static_cast<int>(NativePage::Display));
        if (game::safe_mem::CanAccess(ctx.esp + 4u, 4u)) {
            X86_MEM32(ctx.esp + 4u) = kActionGameOptions;
        }
        LOGI("[OK] options_menu: entering native Display Options screen");
    } else if (action == kActionQualityOptions) {
        g_native_page.store(static_cast<int>(NativePage::Quality));
        if (game::safe_mem::CanAccess(ctx.esp + 4u, 4u)) {
            X86_MEM32(ctx.esp + 4u) = kActionGameOptions;
        }
        LOGI("[OK] options_menu: entering native Quality Options screen");
    } else if (action == kActionCheatOptions) {
        g_native_page.store(static_cast<int>(NativePage::Cheats));
        if (game::safe_mem::CanAccess(ctx.esp + 4u, 4u)) {
            X86_MEM32(ctx.esp + 4u) = kActionGameOptions;
        }
        LOGI("[OK] options_menu: entering native Cheat Options screen");
    } else if (action == kActionGameOptions) {
        g_native_page.store(static_cast<int>(NativePage::None));
    }
    __imp__sub_00163830(ctx, base);
}

// Reset custom screen mode when backing out of child screens.
extern "C" void sub_00163C40(X86Context& ctx, uint8_t* base) {
    g_native_page.store(static_cast<int>(NativePage::None));
    __imp__sub_00163C40(ctx, base);
}

// Populate mcMenuGameOptions with Display, Quality, or Cheat Options rows when active.
extern "C" void sub_0018DD50(X86Context& ctx, uint8_t* base) {
    const auto page = static_cast<NativePage>(g_native_page.load());
    if (page == NativePage::None) {
        __imp__sub_0018DD50(ctx, base);
        return;
    }
    PopulateNativeOptionsList(ctx, base, ctx.ecx, page);
    ApplyNativeScreenTitle(ctx, base, page);
    ctx.esp += 4u;  // __thiscall ret 0
}

// Handle Left (-1) / Right (+1) adjustment or A-button click (+1) on custom Options rows.
extern "C" void sub_0018DB10(X86Context& ctx, uint8_t* base) {
    const auto page = static_cast<NativePage>(g_native_page.load());
    if (page == NativePage::None) {
        __imp__sub_0018DB10(ctx, base);
        return;
    }

    const int32_t raw_dir = static_cast<int32_t>(game::safe_mem::Read32(ctx.esp + 4u, 1u));
    const int step = (raw_dir < 0) ? -1 : 1;

    const uint32_t list_va = GetOptionScreenList();
    if (list_va && g_entry_count > 0u) {
        const uint32_t col = game::safe_mem::Read32(list_va + 0x25Cu, 0u);
        const uint32_t sel_idx = (col < 4u) ? game::safe_mem::Read32(list_va + (col << 4) + 0x268u, 0u) : 0u;
        if (sel_idx < g_entry_count) {
            const NativeEntry& entry = g_entries[sel_idx];
            if (entry.is_cheat) {
                if (entry.cheat_id == game::cheats::CheatId::Cash5000) {
                    game::cheats::TriggerCashCheat();
                } else {
                    game::cheats::ToggleCheat(entry.cheat_id);
                }
            } else {
                (void)app::rows::Value(entry.row, step);
            }
            RefreshVisibleRows(ctx, base);
        }
    }

    ctx.esp += 8u;  // __thiscall ret 0x04 (matches cb_type=2 tail-jump from sub_0020ED50)
}
