// Modular X86 hooks for MC3D-Remix 3D model loaders, texture loaders, and material committers.

#include <cstdint>
#include <string>

#include "cpu/x86_context.h"
#include "cpu/x86_ops.h"
#include "game/deferred_tss_guard.h"
#include "game/guest_mem_safe.h"
#include "game/object_logger.h"
#include "game/render_accuracy.h"
#include "game/render_accuracy_internal.h"
#include "gpu/texture.h"

namespace {

uint32_t ProbeGfxD3dTex(uint32_t gfx_tex_va) {
    if (!gfx_tex_va || !game::safe_mem::CanAccess(gfx_tex_va, 20u)) return 0;
    gpu::TextureDesc direct_desc{};
    if (gpu::Describe(gfx_tex_va, direct_desc) && direct_desc.data) return gfx_tex_va;
    if (!game::safe_mem::CanAccess(gfx_tex_va, 0x24u)) return 0;
    for (uint32_t off : {0x08u, 0x04u, 0x0Cu, 0x10u, 0x14u, 0x18u, 0x1Cu}) {
        const uint32_t cand = game::safe_mem::Read32(gfx_tex_va + off, 0);
        gpu::TextureDesc desc{};
        if (cand && game::safe_mem::CanAccess(cand, 20u) && gpu::Describe(cand, desc) && desc.data) return cand;
    }
    return 0;
}

}  // namespace

// Model/mesh loader: sub_000DEC90(const char* name, ...) -> eax = rmcLod* / model object
extern "C" void __imp__sub_000DEC90(X86Context& ctx, uint8_t* base);
extern "C" void sub_000DEC90(X86Context& ctx, uint8_t* base) {
    const uint32_t name_va = game::safe_mem::Read32(ctx.esp + 4u, 0);
    const std::string name = game::safe_mem::ReadSanitizedCString(name_va, 96u);
    __imp__sub_000DEC90(ctx, base);
    if (ctx.eax || !name.empty()) {
        game::objlog::LogModelLoad("MOD", name.c_str(), ctx.eax, name_va);
    }
}

// Texture loader: sub_002D8D50(const char* name, ...) -> eax = gfxTexture*
extern "C" void __imp__sub_002D8D50(X86Context& ctx, uint8_t* base);
extern "C" void sub_002D8D50(X86Context& ctx, uint8_t* base) {
    const uint32_t name_va = game::safe_mem::Read32(ctx.esp + 4u, 0);
    const std::string name = game::safe_mem::ReadSanitizedCString(name_va, 96u);
    __imp__sub_002D8D50(ctx, base);
    if (ctx.eax || !name.empty()) {
        const uint32_t d3d_tex = ProbeGfxD3dTex(ctx.eax);
        game::objlog::LogModelLoad("TEX", name.c_str(), ctx.eax, d3d_tex);
    }
}

// rmcTextureFactoryXbox::ResourcePageIn(rsc, tex_array, count)
extern "C" void __imp__sub_000E5820(X86Context& ctx, uint8_t* base);
extern "C" void sub_000E5820(X86Context& ctx, uint8_t* base) {
    const uint32_t rsc = game::safe_mem::Read32(ctx.esp + 4u, 0);
    const uint32_t arr = game::safe_mem::Read32(ctx.esp + 8u, 0);
    const uint32_t cnt = game::safe_mem::Read32(ctx.esp + 12u, 0);
    __imp__sub_000E5820(ctx, base);
    game::objlog::LogResourcePageIn("PAGE_TEX", rsc, arr, cnt);
}

// rmcShaderFactoryStandard::ResourcePageIn
extern "C" void __imp__sub_000DFB40(X86Context& ctx, uint8_t* base);
extern "C" void sub_000DFB40(X86Context& ctx, uint8_t* base) {
    const uint32_t a0 = game::safe_mem::Read32(ctx.esp + 4u, 0);
    const uint32_t a1 = game::safe_mem::Read32(ctx.esp + 8u, 0);
    const uint32_t a2 = game::safe_mem::Read32(ctx.esp + 12u, 0);
    __imp__sub_000DFB40(ctx, base);
    game::objlog::LogResourcePageIn("PAGE_SHDR", a0, a1, a2);
}

// Immediate-mode primitive builder wrapping Begin/End (sub_004182E0; D3DDevice_End sub_00417B00 is HLE in d3d8.cpp)
extern "C" void __imp__sub_004182E0(X86Context& ctx, uint8_t* base);
extern "C" void sub_004182E0(X86Context& ctx, uint8_t* base) {
    game::render_accuracy::ScopedGuestTssGuard tss_guard;
    game::objlog::SetDrawHint("IMM_4182E0", ctx.ecx);
    game::render_accuracy::ApplyRenderAccuracy();
    __imp__sub_004182E0(ctx, base);
    game::render_accuracy::InvalidateMc3dStateShadows();
    game::objlog::ClearDrawHint();
}

// MC3 material/pass committers updating deferred TSS (0x4228E0+) and shadow state (0x5594A4+).
// Note: sub_000E2B90 updates MC3D's own RS/TSS caches natively on every draw, so wiping
// all 166 RS cache entries there is unnecessary; keep post-sync on infrequent material commits.
extern "C" void __imp__sub_000E4340(X86Context& ctx, uint8_t* base);
extern "C" void sub_000E4340(X86Context& ctx, uint8_t* base) {
    __imp__sub_000E4340(ctx, base);
    game::render_accuracy::InvalidateMc3dStateShadows();
}

extern "C" void __imp__sub_000E4F30(X86Context& ctx, uint8_t* base);
extern "C" void sub_000E4F30(X86Context& ctx, uint8_t* base) {
    __imp__sub_000E4F30(ctx, base);
    game::render_accuracy::InvalidateMc3dStateShadows();
}

extern "C" void __imp__sub_002D3130(X86Context& ctx, uint8_t* base);
extern "C" void sub_002D3130(X86Context& ctx, uint8_t* base) {
    __imp__sub_002D3130(ctx, base);
    game::render_accuracy::InvalidateMc3dStateShadows();
}
