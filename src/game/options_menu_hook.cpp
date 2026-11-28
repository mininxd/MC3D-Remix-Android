#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "common/menu.h"
#include "common/mouse_keyboard.h"
#include "core/log.h"
#include "cpu/x86_context.h"
#include "cpu/x86_ops.h"
#include "game/guest_mem_safe.h"
#include "kernel/memory.h"

extern "C" void __imp__sub_00154E40(X86Context& ctx, uint8_t* base);
extern "C" void __imp__sub_0018C770(X86Context& ctx, uint8_t* base);
extern "C" void __imp__sub_0018CAD0(X86Context& ctx, uint8_t* base);
extern "C" void __imp__sub_0018D590(X86Context& ctx, uint8_t* base);
extern "C" void __imp__sub_0018D610(X86Context& ctx, uint8_t* base);
extern "C" void __imp__sub_00163830(X86Context& ctx, uint8_t* base);
extern "C" void __imp__sub_00163C40(X86Context& ctx, uint8_t* base);
extern "C" void __imp__sub_0018DD50(X86Context& ctx, uint8_t* base);
extern "C" void __imp__sub_0018DB10(X86Context& ctx, uint8_t* base);
extern "C" void __imp__sub_00348BE0(X86Context& ctx, uint8_t* base);
extern "C" void __imp__sub_00348D70(X86Context& ctx, uint8_t* base);

extern "C" void sub_001395E0(X86Context& ctx, uint8_t* base);
extern "C" void sub_00348950(X86Context& ctx, uint8_t* base);
extern "C" void sub_00348AF0(X86Context& ctx, uint8_t* base);
extern "C" void sub_0018D610(X86Context& ctx, uint8_t* base);

namespace {

constexpr uint32_t kMenuMgrGlobalPtr = 0x0050F148u;
constexpr uint32_t kFlashRootGlobalPtr = 0x0050F15Cu;
constexpr uint32_t kFlashVarTitle = 0x0045B9F8u;        // "title"
constexpr uint32_t kFlashVarSectionName = 0x0045772Cu;  // "sectionName"
constexpr uint32_t kActionAudioOptions = 0x09u;
constexpr uint32_t kActionControllerOptions = 0x0Cu;
constexpr uint32_t kActionGameOptions = 0x41u;
constexpr uint32_t kActionCredits = 0x42u;
constexpr uint32_t kActionDisplayOptions = 0x7F4Bu;
constexpr uint32_t kActionQualityOptions = 0x7F4Cu;
constexpr uint32_t kActionTouchOptions = 0x7F4Eu;
constexpr uint32_t kActionCheatCollections = 0x7F4Fu;
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
    Touch = 4,
    CheatCollections = 5,
};

std::atomic<int> g_native_page{static_cast<int>(NativePage::None)};
std::atomic<bool> g_options_populated_touch_active{false};
std::atomic<int32_t> g_options_controller_idx{-1};

thread_local bool g_in_main_menu_populate = false;
thread_local bool g_skipped_networking = false;

thread_local bool g_in_options_populate = false;
thread_local bool g_skipped_controller = false;
thread_local uint32_t g_options_shift = 0u;
thread_local bool g_inserted_audio_subitems = false;
thread_local bool g_inserted_cheat_collections = false;
thread_local uint32_t g_audio_handle = 0u;
thread_local uint32_t g_audio_list_va = 0u;
thread_local uint32_t g_audio_owner_va = 0u;
thread_local bool g_rebuilding_options = false;

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
        g_skipped_controller = false;
        g_options_shift = 0u;
        g_inserted_audio_subitems = false;
        g_inserted_cheat_collections = false;
        g_audio_handle = 0u;
        g_audio_list_va = 0u;
        g_audio_owner_va = 0u;
    }
    ~ScopedOptionsMenuPopulate() {
        g_in_options_populate = false;
        g_skipped_controller = false;
        g_inserted_cheat_collections = false;
        g_audio_handle = 0u;
        g_audio_list_va = 0u;
        g_audio_owner_va = 0u;
    }
};

struct NativeEntry {
    app::rows::Row row{};
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
    const std::string val = app::rows::Value(e.row, 0);
    if (val.empty()) return std::string(e.row.label);
    return std::string(e.row.label) + ": " + val;
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

bool IsFrontendOptionsMenu() {
    const uint32_t state_mgr_va = game::safe_mem::Read32(0x00557ED0u, 0u);
    return state_mgr_va != 0u && game::safe_mem::Read32(state_mgr_va + 0x04u, 0u) == 7u;
}

void AppendCustomOptionsFallback(X86Context& ctx, uint8_t* base) {
    uint32_t owner_va = 0u;
    const uint32_t list_va = GetOptionScreenList(&owner_va);
    if (!list_va || !owner_va || !game::safe_mem::CanAccess(owner_va, 4u)) return;

    if (!g_inserted_audio_subitems) {
        AppendRootOptionItem(ctx, base, list_va, owner_va, kActionDisplayOptions, "Display Options");
        AppendRootOptionItem(ctx, base, list_va, owner_va, kActionQualityOptions, "Quality Options");
        AppendRootOptionItem(ctx, base, list_va, owner_va, kActionTouchOptions, "Touchscreen Options");
    }
    if (!g_inserted_cheat_collections && IsFrontendOptionsMenu()) {
        AppendRootOptionItem(ctx, base, list_va, owner_va, kActionCheatCollections, "Cheat Collections");
    }
}

int ToAppPage(NativePage page) {
    if (page == NativePage::Quality) return app::rows::kQualityPage;
    if (page == NativePage::Touch) return app::rows::kTouchPage;
    if (page == NativePage::CheatCollections) return app::rows::kCheatCollectionsPage;
    return app::rows::kDisplayPage;
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
    size_t total = 0;
    const app::rows::Row* rows = app::rows::Page(ToAppPage(page), &total);
    for (size_t i = 0; i < total && g_entry_count < list_cap; ++i) {
        if (rows[i].kind == app::rows::Kind::Value || rows[i].kind == app::rows::Kind::Action) {
            NativeEntry entry{};
            entry.row = rows[i];
            g_entries[g_entry_count++] = entry;
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
    // Use single-word titles in Main Menu and full titles in Pause Menu.
    const char* title = nullptr;
    if (flash_root_va) {
        if (page == NativePage::Display) title = "Display";
        else if (page == NativePage::Quality) title = "Quality";
        else if (page == NativePage::CheatCollections) title = "Cheats";
        else title = "Touch";
    } else {
        if (page == NativePage::Display) title = "Display Options";
        else if (page == NativePage::Quality) title = "Quality Options";
        else if (page == NativePage::CheatCollections) title = "Cheat Collections";
        else title = "Touchscreen Options";
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
//  - Options Menu: track Audio Options (0x09) and Cheat Codes (0x32) so custom submenus
//    insert cleanly underneath them with contiguous row indices.
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
        const uint32_t cb_obj = game::safe_mem::Read32(ctx.esp + 16u, 0u);
        const uint32_t cb_fn = game::safe_mem::Read32(ctx.esp + 20u, 0u);
        const uint32_t stored_param = game::safe_mem::Read32(ctx.esp + 24u, 0u);
        const uint32_t list_va = ctx.ecx;
        if (cb_fn == kFnMenuAction && stored_param == kActionControllerOptions) {
            const int32_t orig_idx = game::safe_mem::CanAccess(ctx.esp + 4u, 4u)
                ? static_cast<int32_t>(X86_MEM32(ctx.esp + 4u))
                : 0;
            g_options_controller_idx.store(orig_idx, std::memory_order_relaxed);
            if (g_options_populated_touch_active.load(std::memory_order_relaxed)) {
                g_skipped_controller = true;
                ctx.eax = 0u;
                ctx.esp += 32u;  // return address + 0x1C args
                return;
            }
        }
        if (cb_fn == kFnMenuAction && stored_param == kActionCredits &&
            IsFrontendOptionsMenu() && !g_inserted_cheat_collections &&
            list_va != 0u && cb_obj != 0u) {
            AppendRootOptionItem(ctx, base, list_va, cb_obj, kActionCheatCollections, "Cheat Collections");
            g_inserted_cheat_collections = true;
            ++g_options_shift;
        }
        if (game::safe_mem::CanAccess(ctx.esp + 4u, 4u)) {
            const int32_t idx = static_cast<int32_t>(X86_MEM32(ctx.esp + 4u));
            const int32_t net_shift = static_cast<int32_t>(g_options_shift) - (g_skipped_controller ? 1 : 0);
            const int32_t shifted = idx + net_shift;
            if (shifted >= 0) {
                X86_MEM32(ctx.esp + 4u) = static_cast<uint32_t>(shifted);
            }
        }
        __imp__sub_00348BE0(ctx, base);
        if (cb_fn == kFnMenuAction && ctx.eax != 0u && stored_param == kActionAudioOptions) {
            g_audio_handle = ctx.eax;
            g_audio_list_va = list_va;
            g_audio_owner_va = cb_obj;
        }
        return;
    }
    __imp__sub_00348BE0(ctx, base);
}

// Intercept CMenuList::SetItemText (sub_00348D70: __thiscall ret 0x0C):
//  - Main Menu: skip label assignment for filtered Networking handle.
//  - Options Menu: skip filtered Control Options handle when touchscreen active; right after Audio Options
//    insert Display Options, Quality Options, Touchscreen Options.
extern "C" void sub_00348D70(X86Context& ctx, uint8_t* base) {
    const uint32_t handle = game::safe_mem::Read32(ctx.esp + 4u, 0u);
    if ((g_in_main_menu_populate || g_in_options_populate) && handle == 0u) {
        ctx.esp += 16u;  // return address + 0x0C args
        return;
    }
    __imp__sub_00348D70(ctx, base);

    if (g_in_options_populate && !g_inserted_audio_subitems &&
        g_audio_handle != 0u && handle == g_audio_handle) {
        g_audio_handle = 0u;
        if (g_audio_list_va && g_audio_owner_va) {
            AppendRootOptionItem(ctx, base, g_audio_list_va, g_audio_owner_va,
                                 kActionDisplayOptions, "Display Options");
            AppendRootOptionItem(ctx, base, g_audio_list_va, g_audio_owner_va,
                                 kActionQualityOptions, "Quality Options");
            AppendRootOptionItem(ctx, base, g_audio_list_va, g_audio_owner_va,
                                 kActionTouchOptions, "Touchscreen Options");
            g_inserted_audio_subitems = true;
            g_options_shift += 3u;
        }
    }
}

// Hide Main Menu Xbox Live sign-in status widget ([this + 0x1C0], "Not Signed In" / "Signed in...").
extern "C" void sub_0018CAD0(X86Context& ctx, uint8_t* base) {
    const uint32_t status_widget_va = game::safe_mem::Read32(ctx.ecx + 0x1C0u, 0u);
    CallWidgetSetVisible(ctx, base, status_widget_va, 0u);
    ctx.esp += 4u;  // __thiscall ret 0
}

// Override Options menu populate: hide Control Options on touchscreen, show on external controller,
// and insert Display/Quality/Touchscreen Options underneath Audio Options.
extern "C" void sub_0018D610(X86Context& ctx, uint8_t* base) {
    g_native_page.store(static_cast<int>(NativePage::None));
    g_options_populated_touch_active.store(input::TouchActive(), std::memory_order_relaxed);
    bool inserted_audio = false;
    bool inserted_cheats = false;
    {
        ScopedOptionsMenuPopulate guard;
        __imp__sub_0018D610(ctx, base);
        inserted_audio = g_inserted_audio_subitems;
        inserted_cheats = g_inserted_cheat_collections;
    }
    g_inserted_audio_subitems = inserted_audio;
    g_inserted_cheat_collections = inserted_cheats;
    AppendCustomOptionsFallback(ctx, base);
    g_inserted_audio_subitems = false;
    g_inserted_cheat_collections = false;
}

// Live-rebuild root Options list when switching between touchscreen and external controller while Options is open.
extern "C" void sub_0018D590(X86Context& ctx, uint8_t* base) {
    if (!g_rebuilding_options &&
        g_native_page.load(std::memory_order_relaxed) == static_cast<int>(NativePage::None) &&
        ctx.ecx != 0u && (game::safe_mem::Read8(ctx.ecx + 0x44u, 0u) & 0x01u) != 0u) {
        uint32_t active_owner = 0u;
        const uint32_t list_va = GetOptionScreenList(&active_owner);
        const bool cur_touch = input::TouchActive();
        if (list_va != 0u && active_owner == ctx.ecx &&
            cur_touch != g_options_populated_touch_active.load(std::memory_order_relaxed) &&
            ctx.esp > kScratchBytes + 128u) {
            g_rebuilding_options = true;
            const SavedRegs saved = SavedRegs::Save(ctx);
            const uint32_t active_list = game::safe_mem::Read32(list_va + 0x258u, 0u);
            const uint32_t col = game::safe_mem::Read32(list_va + 0x25Cu, 0u);
            const uint32_t old_sel = (col < 4u) ? game::safe_mem::Read32(list_va + (col << 4) + 0x268u, 0u) : 0u;
            const int32_t ctrl_idx = g_options_controller_idx.load(std::memory_order_relaxed);

            ctx.ecx = list_va;
            X86_PUSH32(1u);
            X86_PUSH32(0u);
            sub_00348950(ctx, base);

            X86_PUSH32(0u);
            sub_0018D610(ctx, base);

            const uint32_t new_count = (active_list < 4u)
                ? game::safe_mem::Read32(list_va + active_list * 4u + 0x238u, 0u)
                : 0u;
            uint32_t new_sel = old_sel;
            if (ctrl_idx >= 0) {
                const uint32_t u_ctrl = static_cast<uint32_t>(ctrl_idx);
                if (cur_touch && old_sel > u_ctrl) --new_sel;
                else if (!cur_touch && old_sel >= u_ctrl) ++new_sel;
            }
            if (new_count > 0u) {
                if (new_sel >= new_count) new_sel = new_count - 1u;
                ctx.ecx = list_va;
                X86_PUSH32(0xFFFFFFFFu);
                X86_PUSH32(new_sel);
                X86_PUSH32(0u);
                sub_00348AF0(ctx, base);
            }
            if (game::safe_mem::CanAccess(list_va + 0x331u, 1u)) {
                X86_MEM8(list_va + 0x331u) = 1u;
            }
            saved.Restore(ctx);
            g_rebuilding_options = false;
        }
    }
    __imp__sub_0018D590(ctx, base);
}

// Route custom Options pages through mcMenuGameOptions host screen (0x41).
extern "C" void sub_00163830(X86Context& ctx, uint8_t* base) {
    const uint32_t action = game::safe_mem::Read32(ctx.esp + 4u, 0u);
    if (action == kActionDisplayOptions) {
        g_native_page.store(static_cast<int>(NativePage::Display));
        if (game::safe_mem::CanAccess(ctx.esp + 4u, 4u)) X86_MEM32(ctx.esp + 4u) = kActionGameOptions;
        LOGI("[OK] options_menu: entering native Display Options screen");
    } else if (action == kActionQualityOptions) {
        g_native_page.store(static_cast<int>(NativePage::Quality));
        if (game::safe_mem::CanAccess(ctx.esp + 4u, 4u)) X86_MEM32(ctx.esp + 4u) = kActionGameOptions;
        LOGI("[OK] options_menu: entering native Quality Options screen");
    } else if (action == kActionTouchOptions) {
        g_native_page.store(static_cast<int>(NativePage::Touch));
        if (game::safe_mem::CanAccess(ctx.esp + 4u, 4u)) X86_MEM32(ctx.esp + 4u) = kActionGameOptions;
        LOGI("[OK] options_menu: entering native Touchscreen Options screen");
    } else if (action == kActionCheatCollections) {
        g_native_page.store(static_cast<int>(NativePage::CheatCollections));
        if (game::safe_mem::CanAccess(ctx.esp + 4u, 4u)) X86_MEM32(ctx.esp + 4u) = kActionGameOptions;
        LOGI("[OK] options_menu: entering native Cheat Collections screen");
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

// Populate mcMenuGameOptions with Display, Quality, or Touchscreen Options rows when active.
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
            (void)app::rows::Value(entry.row, step);

            // Rebuild native rows immediately if an action added/removed a dynamic row (e.g. "Reset hint").
            size_t new_total = 0;
            const app::rows::Row* new_rows = app::rows::Page(ToAppPage(page), &new_total);
            uint32_t new_visible = 0;
            for (size_t i = 0; i < new_total; ++i) {
                if (new_rows[i].kind == app::rows::Kind::Value || new_rows[i].kind == app::rows::Kind::Action) {
                    ++new_visible;
                }
            }
            if (new_visible != g_entry_count) {
                PopulateNativeOptionsList(ctx, base, ctx.ecx, page);
                if (game::safe_mem::CanAccess(list_va + 0x331u, 1u)) {
                    X86_MEM8(list_va + 0x331u) = 1u;
                }
            } else {
                RefreshVisibleRows(ctx, base);
            }
        }
    }

    ctx.esp += 8u;  // __thiscall ret 0x04 (matches cb_type=2 tail-jump from sub_0020ED50)
}
