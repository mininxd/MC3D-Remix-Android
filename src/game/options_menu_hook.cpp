#include <algorithm>
#include <atomic>
#include <cstdint>
#include <string>

#include "common/menu.h"
#include "core/log.h"
#include "cpu/x86_context.h"
#include "cpu/x86_ops.h"
#include "game/guest_mem_safe.h"
#include "kernel/memory.h"

extern "C" void __imp__sub_0018D610(X86Context& ctx, uint8_t* base);
extern "C" void __imp__sub_00163830(X86Context& ctx, uint8_t* base);
extern "C" void __imp__sub_00163C40(X86Context& ctx, uint8_t* base);
extern "C" void __imp__sub_0018DD50(X86Context& ctx, uint8_t* base);
extern "C" void __imp__sub_0018DB10(X86Context& ctx, uint8_t* base);

extern "C" void sub_00348950(X86Context& ctx, uint8_t* base);
extern "C" void sub_00348BE0(X86Context& ctx, uint8_t* base);
extern "C" void sub_00348D70(X86Context& ctx, uint8_t* base);

namespace {

constexpr uint32_t kMenuMgrGlobalPtr = 0x0050F148u;
constexpr uint32_t kActionGameOptions = 0x41u;
constexpr uint32_t kActionDisplayOptions = 0x7F4Bu;
constexpr uint32_t kActionQualityOptions = 0x7F4Cu;
constexpr uint32_t kFnMenuAction = 0x00163830u;
constexpr uint32_t kFnSettingCallback = 0x0018DB10u;
constexpr uint32_t kMaxListItems = 16u;
constexpr uint32_t kMaxRootListItems = 20u;
constexpr uint32_t kScratchBytes = 192u;
constexpr uint32_t kListStructMinBytes = 0x270u;

enum class NativePage : int {
    None = 0,
    Display = 1,
    Quality = 2,
};

std::atomic<int> g_native_page{static_cast<int>(NativePage::None)};

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
    sub_00348D70(ctx, base);

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
    sub_00348BE0(ctx, base);

    const uint32_t item_handle = ctx.eax;
    if (item_handle != 0u && game::safe_mem::CanAccess(item_handle, 16u)) {
        X86_PUSH32(scratch_va);
        X86_PUSH32(2u);
        X86_PUSH32(item_handle);
        ctx.ecx = list_va;
        X86_PUSH32(0u);
        sub_00348D70(ctx, base);
    }

    saved.Restore(ctx);
}

void AppendCustomOptionsItems(X86Context& ctx, uint8_t* base) {
    uint32_t owner_va = 0u;
    const uint32_t list_va = GetOptionScreenList(&owner_va);
    if (!list_va || !owner_va || !game::safe_mem::CanAccess(owner_va, 4u)) return;

    AppendRootOptionItem(ctx, base, list_va, owner_va, kActionDisplayOptions, "Display Options");
    AppendRootOptionItem(ctx, base, list_va, owner_va, kActionQualityOptions, "Quality Options");
}

void PopulateNativeOptionsList(X86Context& ctx, uint8_t* base, uint32_t screen_va, NativePage page) {
    const uint32_t list_va = GetOptionScreenList();
    if (!list_va || !screen_va || ctx.esp <= kScratchBytes + 128u) return;

    const SavedRegs saved = SavedRegs::Save(ctx);

    // Reset CMenuList rows for column 0 via CMenuList::Reset(1) without touching list layout/padding.
    ctx.ecx = list_va;
    X86_PUSH32(1u);
    X86_PUSH32(0u);
    sub_00348950(ctx, base);

    const uint32_t list_cap = std::min<uint32_t>(
        kMaxListItems, std::max<uint32_t>(1u, game::safe_mem::Read32(list_va + 0x230u, kMaxListItems)));

    const int app_page = (page == NativePage::Quality) ? app::rows::kQualityPage : app::rows::kDisplayPage;
    size_t total = 0;
    const app::rows::Row* rows = app::rows::Page(app_page, &total);

    g_entry_count = 0u;
    for (size_t i = 0; i < total && g_entry_count < list_cap; ++i) {
        if (rows[i].kind == app::rows::Kind::Value) {
            g_entries[g_entry_count++] = NativeEntry{rows[i], 0u};
        }
    }

    const uint32_t scratch_va = saved.esp - kScratchBytes;
    if (game::safe_mem::CanAccess(scratch_va, kScratchBytes)) {
        for (uint32_t r = 0; r < g_entry_count; ++r) {
            NativeEntry& e = g_entries[r];
            // Matches AudioOptions (sub_0018F160) & GameOptions (sub_0018DD50):
            // row_type=2 enables Left/Right, cb_type=2 tail-jumps with [cb+8] (-1 left / +1 right) and ret 4.
            ctx.esp = scratch_va;
            X86_PUSH32(2u);
            X86_PUSH32(1u);
            X86_PUSH32(kFnSettingCallback);
            X86_PUSH32(screen_va);
            X86_PUSH32(2u);
            X86_PUSH32(0u);
            X86_PUSH32(r);
            ctx.ecx = list_va;
            X86_PUSH32(0u);
            sub_00348BE0(ctx, base);

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
                sub_00348D70(ctx, base);
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

}  // namespace

// Override Options menu populate: append Display Options and Quality Options items.
extern "C" void sub_0018D610(X86Context& ctx, uint8_t* base) {
    g_native_page.store(static_cast<int>(NativePage::None));
    __imp__sub_0018D610(ctx, base);
    AppendCustomOptionsItems(ctx, base);
}

// Route Display Options and Quality Options through mcMenuGameOptions host screen (0x41).
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

// Populate mcMenuGameOptions with Display or Quality Options rows when active.
extern "C" void sub_0018DD50(X86Context& ctx, uint8_t* base) {
    const auto page = static_cast<NativePage>(g_native_page.load());
    if (page == NativePage::None) {
        __imp__sub_0018DD50(ctx, base);
        return;
    }
    PopulateNativeOptionsList(ctx, base, ctx.ecx, page);
    ctx.esp += 4u;  // __thiscall ret 0
}

// Handle Left (-1) / Right (+1) adjustment on Display or Quality Options rows.
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
            (void)app::rows::Value(g_entries[sel_idx].row, step);
            RefreshVisibleRows(ctx, base);
        }
    }

    ctx.esp += 8u;  // __thiscall ret 0x04 (matches cb_type=2 tail-jump from sub_0020ED50)
}
