// Weak-symbol XDK 5849 hooks for D3D StateBlocks and D3DDevice_GetVertexShader.

#include <cstdint>
#include <mutex>

#include "cpu/x86_context.h"
#include "game/guest_mem_safe.h"
#include "game/stateblock_hle.h"
#include "kernel/kernel.h"
#include "xdk/d3d8.h"

// D3DDevice_BeginStateBlock() - stdcall 0 (ret)
extern "C" void sub_004169C2(X86Context& ctx, uint8_t* base) {
    ctx.eax = game::stateblock::Begin(ctx, base);
    ctx.esp += 4u;
}

// D3DDevice_EndStateBlock(DWORD* pToken) - stdcall 1 (ret 0x04)
extern "C" void sub_004173FD(X86Context& ctx, uint8_t* base) {
    const uint32_t out_token = game::safe_mem::Read32(ctx.esp + 4u, 0u);
    ctx.eax = game::stateblock::End(ctx, base, out_token);
    ctx.esp += 8u;
}

// D3DDevice_CreateStateBlock(D3DSTATEBLOCKTYPE Type, DWORD* pToken) - stdcall 2 (ret 0x08)
extern "C" void sub_004172D2(X86Context& ctx, uint8_t* base) {
    const uint32_t type = game::safe_mem::Read32(ctx.esp + 4u, 1u);
    const uint32_t out_token = game::safe_mem::Read32(ctx.esp + 8u, 0u);
    ctx.eax = game::stateblock::Create(ctx, base, type, out_token);
    ctx.esp += 12u;
}

// D3DDevice_CaptureStateBlock(DWORD Token) - stdcall 1 (ret 0x04)
extern "C" void sub_00416F80(X86Context& ctx, uint8_t* base) {
    const uint32_t token = game::safe_mem::Read32(ctx.esp + 4u, 0u);
    ctx.eax = game::stateblock::Capture(ctx, base, token);
    ctx.esp += 8u;
}

// D3DDevice_ApplyStateBlock(DWORD Token) - stdcall 1 (ret 0x04)
extern "C" void sub_00416DFF(X86Context& ctx, uint8_t* base) {
    const uint32_t token = game::safe_mem::Read32(ctx.esp + 4u, 0u);
    ctx.eax = game::stateblock::Apply(ctx, base, token);
    ctx.esp += 8u;
}

// D3DDevice_DeleteStateBlock(DWORD Token) - stdcall 1 (ret 0x04)
extern "C" void sub_0041723C(X86Context& ctx, uint8_t* base) {
    const uint32_t token = game::safe_mem::Read32(ctx.esp + 4u, 0u);
    ctx.eax = game::stateblock::Delete(ctx, base, token);
    ctx.esp += 8u;
}

// D3DDevice_GetVertexShader(DWORD* pHandle) - stdcall 1 (ret 0x04)
// Called by draw dispatcher sub_002D6830 to save/restore active VS; d3d8.cpp never wrote [pDev+0x798].
extern "C" void sub_00414E90(X86Context& ctx, uint8_t* /*base*/) {
    const uint32_t out_va = game::safe_mem::Read32(ctx.esp + 4u, 0u);
    if (out_va && game::safe_mem::CanAccess(out_va, 4u)) {
        std::lock_guard<std::recursive_mutex> lock(d3d::Lock());
        kernel::W32(out_va, d3d::State().vs_handle);
    }
    ctx.eax = 0u;
    ctx.esp += 8u;
}
