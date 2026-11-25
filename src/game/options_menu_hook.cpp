#include <cstdint>

#include "common/menu.h"
#include "core/log.h"
#include "cpu/x86_context.h"
#include "cpu/x86_ops.h"
#include "game/guest_mem_safe.h"
#include "kernel/memory.h"

extern "C" void __imp__sub_0018D610(X86Context& ctx, uint8_t* base);
extern "C" void __imp__sub_00163830(X86Context& ctx, uint8_t* base);
extern "C" void sub_00348BE0(X86Context& ctx, uint8_t* base);
extern "C" void sub_00348D70(X86Context& ctx, uint8_t* base);

namespace {

constexpr uint32_t kMenuMgrGlobalPtr = 0x0050F148u;
constexpr uint32_t kActionGraphicsOptions = 0x7F4Bu;
constexpr uint32_t kFnMenuAction = 0x00163830u;
constexpr uint32_t kMaxListItems = 20u;
constexpr uint32_t kScratchBytes = 128u;
constexpr uint32_t kListStructMinBytes = 0x270u;
constexpr int kGraphicsPage = 0;

void WriteGuestUtf16Ascii(uint32_t guest_va, const char* ascii) {
    auto* dst = static_cast<uint16_t*>(kernel::HostPtr(guest_va));
    size_t i = 0;
    for (; ascii[i] != '\0'; ++i) {
        dst[i] = static_cast<uint16_t>(static_cast<uint8_t>(ascii[i]));
    }
    dst[i] = 0u;
}

void AppendGraphicsOptionsItem(X86Context& ctx, uint8_t* base) {
    // sub_0018D610 does not take MenuGameOptions in ecx; it reads [[0x0050F148] + 0x0C].
    const uint32_t mgr_va = game::safe_mem::Read32(kMenuMgrGlobalPtr, 0u);
    const uint32_t menu_obj_va = mgr_va ? game::safe_mem::Read32(mgr_va + 0x0Cu, 0u) : 0u;
    const uint32_t list_va = menu_obj_va ? game::safe_mem::Read32(menu_obj_va + 0xFCu, 0u) : 0u;
    const uint32_t owner_va = menu_obj_va ? game::safe_mem::Read32(menu_obj_va + 0x0Cu, 0u) : 0u;
    if (!list_va || !owner_va) return;
    if (!game::safe_mem::CanAccess(list_va, kListStructMinBytes) ||
        !game::safe_mem::CanAccess(owner_va, 4u)) {
        return;
    }

    const uint32_t active_list = game::safe_mem::Read32(list_va + 0x258u, 0xFFFFFFFFu);
    if (active_list >= 4u) return;

    const uint32_t item_idx = game::safe_mem::Read32(list_va + active_list * 4u + 0x238u, kMaxListItems);
    if (item_idx >= kMaxListItems) return;

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
    WriteGuestUtf16Ascii(scratch_va, "Graphics Options");

    // Push CMenuList::AddItem(item_idx, 0, 0, cb[0..3]) right-to-left (stdcall ret 0x1C).
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
        // SetItemText(item_handle, column=2, utf16_label) (stdcall ret 0x0C).
        X86_PUSH32(scratch_va);
        X86_PUSH32(2u);
        X86_PUSH32(item_handle);
        ctx.ecx = list_va;
        X86_PUSH32(0u);
        sub_00348D70(ctx, base);

        static bool logged_add = false;
        if (!logged_add) {
            logged_add = true;
            LOGI("[OK] options_menu: added 'Graphics Options' item (idx=%u list=0x%08X owner=0x%08X)",
                 item_idx, list_va, owner_va);
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

}  // namespace

// Override MenuGameOptions::OnEnter to append a native "Graphics Options" button.
extern "C" void sub_0018D610(X86Context& ctx, uint8_t* base) {
    __imp__sub_0018D610(ctx, base);
    AppendGraphicsOptionsItem(ctx, base);
}

// Intercept custom Graphics Options selection before MC3D treats action ID as a screen ID.
extern "C" void sub_00163830(X86Context& ctx, uint8_t* base) {
    if (game::safe_mem::Read32(ctx.esp + 4u, 0u) == kActionGraphicsOptions) {
        LOGI("[OK] options_menu: opening Graphics Options panel");
        app::OpenMenuPage(kGraphicsPage);
        ctx.esp += 8u;  // stdcall ret 0x04
        return;
    }
    __imp__sub_00163830(ctx, base);
}
