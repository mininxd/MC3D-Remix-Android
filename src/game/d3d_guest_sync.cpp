#include "game/d3d_guest_sync.h"

#include <cstring>

#include "game/guest_mem_safe.h"
#include "game/render_accuracy_internal.h"
#include "kernel/kernel.h"

namespace game {
namespace d3d_sync {

namespace {

constexpr uint32_t kDummySimpleMethod = 0x1D90u;

}  // namespace

uint32_t SimpleMethodForRs(uint32_t rs) {
    if (rs >= gpu::kRsDeferredFirst) return 0u;
    const uint32_t enc = safe_mem::Read32(kD3dSimpleEncodeTable + rs * 4u, 0u);
    const uint32_t method = enc & 0x1FFCu;
    return (method == kDummySimpleMethod) ? 0u : method;
}

uint32_t D3d8TransformType(uint32_t xdk_type) {
    if (xdk_type < 2u) return xdk_type + 2u;
    if (xdk_type < 6u) return xdk_type + 14u;
    return xdk_type + 250u;
}

void PullGuestDeferredToHost(gpu::State& s) {
    for (uint32_t st = 0; st < 4u; ++st) {
        for (uint32_t slot = 0; slot < 22u; ++slot) {
            const uint32_t ea = render_accuracy::addr::kD3dDeferredTss + st * 0x80u + slot * 4u;
            if (safe_mem::CanAccess(ea, 4u)) s.tss[st][slot] = kernel::R32(ea);
        }
        const uint32_t tci_ea =
            render_accuracy::addr::kD3dDeferredTss + st * 0x80u + gpu::kTssTexCoordIndex * 4u;
        if (safe_mem::CanAccess(tci_ea, 4u)) s.tss[st][gpu::kTssTexCoordIndex] = kernel::R32(tci_ea);
    }
    for (uint32_t rs = gpu::kRsDeferredFirst; rs < gpu::kRsComplexFirst; ++rs) {
        const uint32_t ea = render_accuracy::addr::kD3dRenderState + rs * 4u;
        if (safe_mem::CanAccess(ea, 4u)) s.rs[rs] = kernel::R32(ea);
    }
}

void MirrorHostStateToGuest(const gpu::State& s) {
    const uint32_t pDev = safe_mem::Read32(render_accuracy::addr::kD3dDevicePtr, 0u);
    if (pDev && safe_mem::CanAccess(pDev + kDevVertexShaderHnd, 4u)) {
        const uint32_t ps_h = s.ps ? s.ps->handle : 0u;
        if (kernel::R32(pDev + render_accuracy::addr::kDevPixelShader) != ps_h) {
            kernel::W32(pDev + render_accuracy::addr::kDevPixelShader, ps_h);
        }
        if (kernel::R32(pDev + kDevVertexShaderHnd) != s.vs_handle) {
            kernel::W32(pDev + kDevVertexShaderHnd, s.vs_handle);
        }
        const uint32_t vs_ptr =
            ((s.vs_handle & 1u) && (s.vs_handle & ~1u)) ? (s.vs_handle & ~1u) : kD3dFvfShaderStruct;
        if (kernel::R32(pDev + kDevVertexShaderPtr) != vs_ptr) {
            kernel::W32(pDev + kDevVertexShaderPtr, vs_ptr);
        }
    }

    if (!safe_mem::CanAccess(render_accuracy::addr::kD3dRenderState, gpu::kRsDeferredFirst * 4u)) {
        return;
    }
    const uint32_t start_rs = s.ps ? 0u : 57u;
    auto* guest_rs = static_cast<uint32_t*>(kernel::HostPtr(render_accuracy::addr::kD3dRenderState));
    for (uint32_t rs = start_rs; rs < gpu::kRsDeferredFirst; ++rs) {
        const uint32_t method = SimpleMethodForRs(rs);
        if (!method) continue;
        const uint32_t idx = method >> 2;
        if (idx < s.simple.size() && guest_rs[rs] != s.simple[idx]) {
            guest_rs[rs] = s.simple[idx];
        }
    }
}

uint32_t ReadRenderState(const gpu::State& s, uint32_t rs) {
    if (rs < gpu::kRsDeferredFirst) {
        const uint32_t method = SimpleMethodForRs(rs);
        if (method && (method >> 2) < s.simple.size()) return s.simple[method >> 2];
        return safe_mem::Read32(render_accuracy::addr::kD3dRenderState + rs * 4u, 0u);
    }
    if (rs < gpu::kRsMax) return s.rs[rs];
    return 0u;
}

void ApplyRenderState(gpu::State& s, uint32_t rs, uint32_t val) {
    if (rs >= gpu::kRsMax) return;
    if (rs < gpu::kRsDeferredFirst) {
        const uint32_t method = SimpleMethodForRs(rs);
        if (method && (method >> 2) < s.simple.size()) {
            s.simple[method >> 2] = val;
        }
    } else {
        s.rs[rs] = val;
    }
    const uint32_t ea = render_accuracy::addr::kD3dRenderState + rs * 4u;
    if (safe_mem::CanAccess(ea, 4u)) kernel::W32(ea, val);
}

void ApplyTextureStageState(gpu::State& s, uint32_t stage, uint32_t slot, uint32_t val) {
    if (stage >= 4u || slot >= gpu::kTssCount) return;
    s.tss[stage][slot] = val;
    const uint32_t ea = render_accuracy::addr::kD3dDeferredTss + stage * 0x80u + slot * 4u;
    if (safe_mem::CanAccess(ea, 4u)) kernel::W32(ea, val);
}

void LoadPixelShaderIntoState(gpu::State& s, const std::shared_ptr<gpu::PixelShader>& ps) {
    s.ps = ps;
    const uint32_t pDev = safe_mem::Read32(render_accuracy::addr::kD3dDevicePtr, 0u);
    if (pDev && safe_mem::CanAccess(pDev + render_accuracy::addr::kDevPixelShader, 4u)) {
        kernel::W32(pDev + render_accuracy::addr::kDevPixelShader, ps ? ps->handle : 0u);
    }
    if (!ps) return;

    const uint32_t* d = ps->def;
    auto put = [&](uint32_t method, uint32_t v) { s.simple[method >> 2] = v; };
    for (uint32_t i = 0; i < 8u; ++i) {
        put(gpu::kMPsAlphaInputs + i * 4u, d[0u + i]);
        put(gpu::kMPsConstant0 + i * 4u, d[10u + i]);
        put(gpu::kMPsConstant1 + i * 4u, d[18u + i]);
        put(gpu::kMPsAlphaOutputs + i * 4u, d[26u + i]);
        put(gpu::kMPsRgbInputs + i * 4u, d[34u + i]);
        put(gpu::kMPsRgbOutputs + i * 4u, d[45u + i]);
    }
    put(gpu::kMPsFinalAbcd, d[8]);
    put(gpu::kMPsFinalEfgh, d[9]);
    put(gpu::kMPsCompareMode, d[42]);
    put(gpu::kMPsFinalConstant0, d[43]);
    put(gpu::kMPsFinalConstant1, d[44]);
    put(gpu::kMPsCombinerCount, d[53]);
    put(gpu::kMPsDotMapping, d[55]);
    put(gpu::kMPsInputTexture, d[56]);
    ApplyRenderState(s, gpu::kRsPsTextureModes, d[54]);

    if (safe_mem::CanAccess(render_accuracy::addr::kD3dRenderState, 57u * 4u)) {
        std::memcpy(kernel::HostPtr(render_accuracy::addr::kD3dRenderState), d, 57u * 4u);
    }
}

}  // namespace d3d_sync
}  // namespace game
