#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "common/menu.h"
#include "common/translations.h"
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
extern "C" void sub_00163C40(X86Context& ctx, uint8_t* base);
extern "C" void sub_0018DD50(X86Context& ctx, uint8_t* base);

namespace {

constexpr uint32_t kMenuMgrGlobalPtr = 0x0050F148u;
constexpr uint32_t kActionGameOptions = 0x41u;
constexpr uint32_t kActionGraphicsOptions = 0x7F4Bu;
constexpr uint32_t kFnMenuAction = 0x00163830u;
constexpr uint32_t kFnGraphicsCallback = 0x0018DB10u;
constexpr uint32_t kMaxListItems = 16u;
constexpr uint32_t kMaxRootListItems = 20u;
constexpr uint32_t kScratchBytes = 192u;
constexpr uint32_t kListStructMinBytes = 0x270u;
constexpr int kGraphicsPage = 0;

std::atomic<bool> g_graphics_active{false};
std::atomic<int> g_section{0};

enum class EntryKind : uint8_t {
    SectionSwitch,
    Setting,
    RestoreDefaults,
};

struct NativeEntry {
    EntryKind kind = EntryKind::RestoreDefaults;
    app::rows::Row row{};
    uint32_t handle = 0u;
};

NativeEntry g_entries[kMaxListItems]{};
uint32_t g_entry_count = 0u;
constexpr const char* kSectionNames[3] = {"DISPLAY", "PERFORMANCE", "QUALITY"};

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

std::string FormatSliderBar(float ratio) {
    constexpr int kSlots = 10;
    const int filled = std::clamp(int(std::lround(std::clamp(ratio, 0.0f, 1.0f) * kSlots)), 0, kSlots);
    std::string bar = "[";
    for (int i = 0; i < kSlots; ++i) bar += (i < filled ? '=' : '-');
    bar += "]";
    return bar;
}

std::string FormatEntryLabel(const NativeEntry& e) {
    if (e.kind == EntryKind::SectionSwitch) {
        const int sec = std::clamp(g_section.load(), 0, 2);
        return "Section: < " + app::Ui(kSectionNames[sec], false) + " (" + std::to_string(sec + 1) + "/3) >";
    }
    if (e.kind == EntryKind::RestoreDefaults) {
        return app::Ui("RESTORE DEFAULTS", false);
    }

    const std::string label = app::Ui(e.row.label, false);
    const std::string val = app::Ui(app::rows::Value(e.row, 0), false);
    const bool enabled = app::rows::Enabled(e.row);
    float ratio = 0.0f;
    if (app::rows::SliderRatio(e.row.id, &ratio)) {
        std::string out = label + ": " + FormatSliderBar(ratio) + " " + val;
        if (!enabled) out += " (Auto)";
        return out;
    }
    if (!enabled) return label + ": (" + val + ")";
    return label + ": < " + val + " >";
}

void UpdateListRowText(X86Context& ctx, uint8_t* base, uint32_t list_va, uint32_t handle, const std::string& text) {
    if (!handle || !game::safe_mem::CanAccess(handle, 16u)) return;
    if (ctx.esp <= kScratchBytes + 64u) return;
    const uint32_t saved_esp = ctx.esp;
    const uint32_t saved_eax = ctx.eax;
    const uint32_t saved_ebx = ctx.ebx;
    const uint32_t saved_ecx = ctx.ecx;
    const uint32_t saved_edx = ctx.edx;
    const uint32_t scratch_va = saved_esp - kScratchBytes;
    if (!game::safe_mem::CanAccess(scratch_va, kScratchBytes)) return;

    ctx.esp = scratch_va;
    WriteGuestUtf16Ascii(scratch_va, text.c_str(), kScratchBytes / 2u);
    X86_PUSH32(scratch_va);
    X86_PUSH32(2u);
    X86_PUSH32(handle);
    ctx.ecx = list_va;
    X86_PUSH32(0u);
    sub_00348D70(ctx, base);

    ctx.esp = saved_esp;
    ctx.eax = saved_eax;
    ctx.ebx = saved_ebx;
    ctx.ecx = saved_ecx;
    ctx.edx = saved_edx;
}

void AppendGraphicsOptionsItem(X86Context& ctx, uint8_t* base) {
    uint32_t owner_va = 0u;
    const uint32_t list_va = GetOptionScreenList(&owner_va);
    if (!list_va || !owner_va || !game::safe_mem::CanAccess(owner_va, 4u)) return;

    const uint32_t active_list = game::safe_mem::Read32(list_va + 0x258u, 0xFFFFFFFFu);
    if (active_list >= 4u) return;

    const uint32_t item_idx = game::safe_mem::Read32(list_va + active_list * 4u + 0x238u, kMaxRootListItems);
    if (item_idx >= kMaxRootListItems) return;

    if (ctx.esp <= kScratchBytes + 64u) return;
    const uint32_t saved_esp = ctx.esp;
    const uint32_t saved_eax = ctx.eax;
    const uint32_t saved_ebx = ctx.ebx;
    const uint32_t saved_ecx = ctx.ecx;
    const uint32_t saved_edx = ctx.edx;
    const uint32_t saved_esi = ctx.esi;
    const uint32_t saved_edi = ctx.edi;
    const uint32_t saved_ebp = ctx.ebp;

    const uint32_t scratch_va = saved_esp - kScratchBytes;
    if (!game::safe_mem::CanAccess(scratch_va, kScratchBytes)) return;

    ctx.esp = scratch_va;
    WriteGuestUtf16Ascii(scratch_va, "Graphics Options", kScratchBytes / 2u);

    X86_PUSH32(2u);
    X86_PUSH32(kActionGraphicsOptions);
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

    ctx.esp = saved_esp;
    ctx.eax = saved_eax;
    ctx.ebx = saved_ebx;
    ctx.ecx = saved_ecx;
    ctx.edx = saved_edx;
    ctx.esi = saved_esi;
    ctx.edi = saved_edi;
    ctx.ebp = saved_ebp;
}

void PopulateNativeGraphicsList(X86Context& ctx, uint8_t* base, uint32_t screen_va) {
    const uint32_t list_va = GetOptionScreenList();
    if (!list_va || !screen_va || ctx.esp <= kScratchBytes + 128u) return;

    const uint32_t saved_esp = ctx.esp;
    const uint32_t saved_eax = ctx.eax;
    const uint32_t saved_ebx = ctx.ebx;
    const uint32_t saved_ecx = ctx.ecx;
    const uint32_t saved_edx = ctx.edx;
    const uint32_t saved_esi = ctx.esi;
    const uint32_t saved_edi = ctx.edi;
    const uint32_t saved_ebp = ctx.ebp;

    // Reset CMenuList rows for column 0 via CMenuList::Reset(1) and scroll top to 0.
    ctx.ecx = list_va;
    X86_PUSH32(1u);
    X86_PUSH32(0u);
    sub_00348950(ctx, base);
    if (game::safe_mem::CanAccess(list_va + 0x4Cu, 4u)) {
        X86_MEM32(list_va + 0x4Cu) = 0u;
    }

    const uint32_t list_cap = std::min<uint32_t>(
        kMaxListItems, std::max<uint32_t>(1u, game::safe_mem::Read32(list_va + 0x230u, kMaxListItems)));

    size_t total = 0;
    const app::rows::Row* rows = app::rows::Page(kGraphicsPage, &total);
    const int target_sec = std::clamp(g_section.load(), 0, 2);

    g_entry_count = 0u;
    auto push_entry = [&](const NativeEntry& entry) {
        if (g_entry_count < list_cap) g_entries[g_entry_count++] = entry;
    };

    push_entry(NativeEntry{EntryKind::SectionSwitch, {}, 0u});

    int cur_sec = -1;
    for (size_t i = 0; i < total; ++i) {
        if (rows[i].kind == app::rows::Kind::Heading) {
            ++cur_sec;
            continue;
        }
        if (cur_sec == target_sec && rows[i].kind == app::rows::Kind::Value &&
            std::strcmp(rows[i].label, "HDR") != 0) {
            if (g_entry_count + 1u < list_cap) {
                push_entry(NativeEntry{EntryKind::Setting, rows[i], 0u});
            }
        }
    }

    push_entry(NativeEntry{EntryKind::RestoreDefaults, {}, 0u});

    const uint32_t scratch_va = saved_esp - kScratchBytes;
    if (game::safe_mem::CanAccess(scratch_va, kScratchBytes)) {
        for (uint32_t r = 0; r < g_entry_count; ++r) {
            NativeEntry& e = g_entries[r];
            // Flags: 3 = allows both Left/Right (bit 1) and A button (!= 2); 4 = A button action only.
            const bool adjust_lr = (e.kind == EntryKind::SectionSwitch || e.kind == EntryKind::Setting);
            const uint32_t row_type = adjust_lr ? 2u : 0u;
            const uint32_t cb_flags = adjust_lr ? 3u : 4u;

            ctx.esp = scratch_va;
            X86_PUSH32(cb_flags);
            X86_PUSH32(0u);
            X86_PUSH32(kFnGraphicsCallback);
            X86_PUSH32(screen_va);
            X86_PUSH32(row_type);
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

    ctx.esp = saved_esp;
    ctx.eax = saved_eax;
    ctx.ebx = saved_ebx;
    ctx.ecx = saved_ecx;
    ctx.edx = saved_edx;
    ctx.esi = saved_esi;
    ctx.edi = saved_edi;
    ctx.ebp = saved_ebp;
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

// Override Options menu populate: append "Graphics Options" button and reset active flag when on parent screen.
extern "C" void sub_0018D610(X86Context& ctx, uint8_t* base) {
    g_graphics_active.store(false);
    __imp__sub_0018D610(ctx, base);
    AppendGraphicsOptionsItem(ctx, base);
}

// Intercept Options action dispatch: route Graphics Options cleanly into mcMenuGameOptions host screen (0x41).
extern "C" void sub_00163830(X86Context& ctx, uint8_t* base) {
    const uint32_t action = game::safe_mem::Read32(ctx.esp + 4u, 0u);
    if (action == kActionGraphicsOptions) {
        g_graphics_active.store(true);
        if (game::safe_mem::CanAccess(ctx.esp + 4u, 4u)) {
            X86_MEM32(ctx.esp + 4u) = kActionGameOptions;
        }
        LOGI("[OK] options_menu: entering native Graphics Options screen");
    } else if (action == kActionGameOptions) {
        g_graphics_active.store(false);
    }
    __imp__sub_00163830(ctx, base);
}

// Reset Graphics Options mode when backing out of child screens.
extern "C" void sub_00163C40(X86Context& ctx, uint8_t* base) {
    g_graphics_active.store(false);
    __imp__sub_00163C40(ctx, base);
}

// Populate mcMenuGameOptions with native Graphics Options rows when active.
extern "C" void sub_0018DD50(X86Context& ctx, uint8_t* base) {
    if (!g_graphics_active.load()) {
        __imp__sub_0018DD50(ctx, base);
        return;
    }
    PopulateNativeGraphicsList(ctx, base, ctx.ecx);
    ctx.esp += 4u;  // __thiscall ret 0
}

// Handle Left / Right / A selection on Graphics Options rows.
extern "C" void sub_0018DB10(X86Context& ctx, uint8_t* base) {
    if (!g_graphics_active.load()) {
        __imp__sub_0018DB10(ctx, base);
        return;
    }

    const uint32_t screen_va = ctx.ecx;
    const int32_t raw_dir = static_cast<int32_t>(game::safe_mem::Read32(ctx.esp + 4u, 0u));
    const int step = (raw_dir < 0) ? -1 : 1;

    const uint32_t list_va = GetOptionScreenList();
    if (list_va && g_entry_count > 0u) {
        const uint32_t col = game::safe_mem::Read32(list_va + 0x25Cu, 0u);
        const uint32_t sel_idx = (col < 4u) ? game::safe_mem::Read32(list_va + (col << 4) + 0x268u, 0u) : 0u;
        if (sel_idx < g_entry_count) {
            const NativeEntry& e = g_entries[sel_idx];
            if (e.kind == EntryKind::SectionSwitch) {
                g_section.store((g_section.load() + step + 3) % 3);
                PopulateNativeGraphicsList(ctx, base, screen_va);
                ctx.esp += 8u;  // __thiscall ret 0x04
                return;
            }
            if (e.kind == EntryKind::Setting) {
                if (app::rows::Enabled(e.row)) {
                    (void)app::rows::Value(e.row, step);
                    RefreshVisibleRows(ctx, base);
                }
                ctx.esp += 8u;
                return;
            }
            if (e.kind == EntryKind::RestoreDefaults) {
                app::rows::Defaults(kGraphicsPage);
                RefreshVisibleRows(ctx, base);
                ctx.esp += 8u;
                return;
            }
        }
    }

    ctx.esp += 8u;  // __thiscall ret 0x04
}
