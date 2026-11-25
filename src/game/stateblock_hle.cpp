#include "game/stateblock_hle.h"

#include <array>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "core/log.h"
#include "cpu/x86_context.h"
#include "cpu/x86_ops.h"
#include "game/d3d_guest_sync.h"
#include "game/guest_mem_safe.h"
#include "game/render_accuracy_internal.h"
#include "gpu/state.h"
#include "gpu/texture.h"
#include "kernel/kernel.h"
#include "xdk/d3d8.h"

// HLE / XDK entry points called to keep CDevice refcounts and viewport constants exact.
extern "C" void sub_004123D0(X86Context& ctx, uint8_t* base);  // D3DDevice_SetTexture(stage, tex)
extern "C" void sub_00412610(X86Context& ctx, uint8_t* base);  // D3DDevice_SetIndices(ib, base)
extern "C" void sub_00415130(X86Context& ctx, uint8_t* base);  // D3DDevice_SetStreamSource(n, vb, stride)
extern "C" void sub_00411D80(X86Context& ctx, uint8_t* base);  // D3DDevice_SetViewport(vp)
extern "C" void sub_004145D0(X86Context& ctx, uint8_t* base);  // D3DResource_AddRef(res)
extern "C" void sub_00414610(X86Context& ctx, uint8_t* base);  // D3DResource_Release(res)

namespace game {
namespace stateblock {

namespace {

// XDK 5849 StateBlock recording flag array at 0x00425518 (540 bytes = 0x87 dwords).
constexpr uint32_t kRecTableBase   = 0x00425518u;
constexpr uint32_t kRecTableBytes  = 0x21Cu;
constexpr uint32_t kOffTextures    = 0x000u;  // 4 bytes
constexpr uint32_t kOffPs          = 0x004u;  // 1 byte
constexpr uint32_t kOffVs          = 0x005u;  // 1 byte
constexpr uint32_t kOffIndices     = 0x006u;  // 1 byte
constexpr uint32_t kOffStreams     = 0x007u;  // 16 bytes
constexpr uint32_t kOffPsc         = 0x017u;  // 16 bytes
constexpr uint32_t kOffVsc         = 0x027u;  // 192 bytes
constexpr uint32_t kOffRs          = 0x0E7u;  // 166 bytes
constexpr uint32_t kOffTss         = 0x18Du;  // 32 * 4 bytes ([slot * 4 + stage])
constexpr uint32_t kOffTransforms  = 0x20Du;  // 10 bytes
constexpr uint32_t kOffViewport    = 0x217u;  // 1 byte
constexpr uint32_t kOffMaterial    = 0x218u;  // 1 byte

// XDK 5849 CreateStateBlock(D3DSBT_PIXELSTATE / D3DSBT_VERTEXSTATE) exact tables from default.xbe.
constexpr uint8_t kPixelStateRs[32] = {
    143, 139, 66, 64, 60, 62, 63, 57, 61, 58, 65, 94, 95, 96, 151, 59,
    149, 144, 145, 68, 69, 70, 71, 72, 73, 148, 98, 99, 100, 101, 67, 74
};
constexpr uint8_t kPixelStateTss[27] = {
    12, 14, 15, 16, 18, 19, 22, 23, 25, 24, 28, 0, 1, 29, 3, 4,
    5, 6, 7, 8, 26, 27, 21, 2, 13, 17, 20
};
constexpr uint8_t kVertexStateRs[33] = {
    66, 103, 147, 92, 138, 93, 94, 95, 96, 97, 115, 105, 102, 142, 104, 113,
    112, 111, 110, 137, 116, 117, 118, 119, 120, 121, 122, 152, 153, 124, 125, 123, 82
};
constexpr uint8_t kVertexStateTss[2] = {28, 21};

struct HostStateBlock {
    uint8_t tex_mask = 0;
    bool has_palettes = false;
    bool has_ps = false;
    bool has_vs = false;
    bool has_indices = false;
    uint16_t stream_mask = 0;
    uint16_t psc_mask = 0;
    bool apply_psc_mapping = false;  // true for Begin/End recorded blocks, false for CreateStateBlock
    std::array<uint8_t, 192> vsc_mask{};
    std::array<uint8_t, gpu::kRsMax> rs_mask{};
    std::array<std::array<uint8_t, gpu::kTssCount>, 4> tss_mask{};
    uint16_t transform_mask = 0;
    bool has_viewport = false;
    bool has_material = false;
    uint8_t light_mask = 0;
    uint8_t light_enable_mask = 0;

    std::array<uint32_t, 4> textures{};
    std::array<uint32_t, 4> palettes{};
    std::shared_ptr<gpu::PixelShader> ps;
    uint32_t vs_handle = 0;
    std::shared_ptr<gpu::VertexShader> vs;
    uint32_t index_buffer = 0;
    uint32_t index_data = 0;
    uint32_t base_vertex = 0;
    std::array<gpu::State::Stream, 16> streams{};
    std::array<std::array<float, 4>, 16> psc{};
    std::array<std::array<float, 4>, 192> vsc{};
    std::array<uint32_t, gpu::kRsMax> rs_vals{};
    std::array<std::array<uint32_t, gpu::kTssCount>, 4> tss_vals{};
    std::array<std::array<float, 16>, 10> transforms{};
    gpu::Viewport viewport{};
    std::array<float, 17> material{};
    std::array<std::array<uint32_t, 26>, 8> lights{};
    uint32_t light_enable = 0;
};

struct RecordingContext {
    bool active = false;
    gpu::State saved_state{};
    std::array<uint32_t, 166> saved_guest_rs{};
    std::array<uint32_t, 128> saved_guest_tss{};
    uint8_t touched_lights = 0;
    uint8_t touched_light_enables = 0;
};

std::mutex g_sb_mu;
std::unordered_map<uint32_t, std::shared_ptr<HostStateBlock>> g_blocks;
uint32_t g_next_token = 0x5B000001u;
std::atomic<bool> g_recording_flag{false};
RecordingContext g_rec;
std::atomic<uint64_t> g_created_count{0};
std::atomic<uint64_t> g_applied_count{0};

void CallStdcall1(X86Context& ctx, uint8_t* base, void (*fn)(X86Context&, uint8_t*), uint32_t a0) {
    const uint32_t eax = ctx.eax, ecx = ctx.ecx, edx = ctx.edx;
    X86_PUSH32(a0);
    X86_PUSH32(0u);
    fn(ctx, base);
    ctx.eax = eax;
    ctx.ecx = ecx;
    ctx.edx = edx;
}

void CallStdcall2(X86Context& ctx, uint8_t* base, void (*fn)(X86Context&, uint8_t*),
                  uint32_t a0, uint32_t a1) {
    const uint32_t eax = ctx.eax, ecx = ctx.ecx, edx = ctx.edx;
    X86_PUSH32(a1);
    X86_PUSH32(a0);
    X86_PUSH32(0u);
    fn(ctx, base);
    ctx.eax = eax;
    ctx.ecx = ecx;
    ctx.edx = edx;
}

void CallStdcall3(X86Context& ctx, uint8_t* base, void (*fn)(X86Context&, uint8_t*),
                  uint32_t a0, uint32_t a1, uint32_t a2) {
    const uint32_t eax = ctx.eax, ecx = ctx.ecx, edx = ctx.edx;
    X86_PUSH32(a2);
    X86_PUSH32(a1);
    X86_PUSH32(a0);
    X86_PUSH32(0u);
    fn(ctx, base);
    ctx.eax = eax;
    ctx.ecx = ecx;
    ctx.edx = edx;
}

void AddRefResource(X86Context& ctx, uint8_t* base, uint32_t res_va) {
    if (res_va && safe_mem::CanAccess(res_va, 16u)) {
        CallStdcall1(ctx, base, sub_004145D0, res_va);
    }
}

void ReleaseResource(X86Context& ctx, uint8_t* base, uint32_t res_va) {
    if (res_va && safe_mem::CanAccess(res_va, 16u)) {
        CallStdcall1(ctx, base, sub_00414610, res_va);
    }
}

void ReleaseBlockResources(X86Context& ctx, uint8_t* base, const HostStateBlock& blk) {
    for (uint32_t i = 0; i < 4u; ++i) {
        if ((blk.tex_mask & (1u << i)) && blk.textures[i]) {
            ReleaseResource(ctx, base, blk.textures[i]);
        }
    }
    if (blk.has_indices && blk.index_buffer) {
        ReleaseResource(ctx, base, blk.index_buffer);
    }
    for (uint32_t i = 0; i < 16u; ++i) {
        if ((blk.stream_mask & (1u << i)) && blk.streams[i].vb) {
            ReleaseResource(ctx, base, blk.streams[i].vb);
        }
    }
}

void CaptureValuesIntoBlock(X86Context& ctx, uint8_t* base, HostStateBlock& blk,
                            bool release_old_refs) {
    if (release_old_refs) ReleaseBlockResources(ctx, base, blk);

    gpu::State snap;
    {
        std::lock_guard<std::recursive_mutex> lock(d3d::Lock());
        gpu::State& s = d3d::State();
        d3d_sync::PullGuestDeferredToHost(s);
        snap = s;
    }

    for (uint32_t i = 0; i < 4u; ++i) {
        if (blk.tex_mask & (1u << i)) {
            blk.textures[i] = snap.textures[i];
            AddRefResource(ctx, base, blk.textures[i]);
        }
    }
    if (blk.has_palettes) blk.palettes = snap.palettes;
    if (blk.has_ps) blk.ps = snap.ps;
    if (blk.has_vs) {
        blk.vs_handle = snap.vs_handle;
        blk.vs = snap.vs;
    }
    if (blk.has_indices) {
        blk.index_buffer = snap.index_buffer;
        blk.index_data = snap.index_data;
        blk.base_vertex = snap.base_vertex;
        AddRefResource(ctx, base, blk.index_buffer);
    }
    for (uint32_t i = 0; i < 16u; ++i) {
        if (blk.stream_mask & (1u << i)) {
            blk.streams[i] = snap.streams[i];
            AddRefResource(ctx, base, blk.streams[i].vb);
        }
    }
    for (uint32_t i = 0; i < 16u; ++i) {
        if (blk.psc_mask & (1u << i)) blk.psc[i] = snap.psc[i];
    }
    for (uint32_t i = 0; i < 192u; ++i) {
        if (blk.vsc_mask[i]) blk.vsc[i] = snap.vsc[i];
    }
    for (uint32_t r = 0; r < gpu::kRsMax; ++r) {
        if (!blk.rs_mask[r]) continue;
        if (!snap.ps && (r <= 56u || r == gpu::kRsPsTextureModes)) continue;
        blk.rs_vals[r] = d3d_sync::ReadRenderState(snap, r);
    }
    for (uint32_t st = 0; st < 4u; ++st) {
        for (uint32_t slot = 0; slot < gpu::kTssCount; ++slot) {
            if (blk.tss_mask[st][slot]) blk.tss_vals[st][slot] = snap.tss[st][slot];
        }
    }
    for (uint32_t t = 0; t < 10u; ++t) {
        if (blk.transform_mask & (1u << t)) {
            blk.transforms[t] = snap.transforms[d3d_sync::D3d8TransformType(t)];
        }
    }
    if (blk.has_viewport) blk.viewport = snap.viewport;
    if (blk.has_material) blk.material = snap.material;
    for (uint32_t i = 0; i < 8u; ++i) {
        if (blk.light_mask & (1u << i)) blk.lights[i] = snap.lights[i];
    }
    if (blk.light_enable_mask) blk.light_enable = snap.light_enable;
}

void ApplyBlockToDevice(X86Context& ctx, uint8_t* base, const HostStateBlock& blk) {
    // 1. Resource bindings via D3D HLE entry points so CDevice internal refcounts remain exact.
    for (uint32_t i = 0; i < 4u; ++i) {
        if (!(blk.tex_mask & (1u << i))) continue;
        const uint32_t tex = blk.textures[i];
        if (!tex || safe_mem::CanAccess(tex, 20u)) {
            CallStdcall2(ctx, base, sub_004123D0, i, tex);
        }
    }
    if (blk.has_indices) {
        const uint32_t ib = blk.index_buffer;
        if (!ib || safe_mem::CanAccess(ib, 16u)) {
            CallStdcall2(ctx, base, sub_00412610, ib, blk.base_vertex);
        }
    }
    for (uint32_t i = 0; i < 16u; ++i) {
        if (!(blk.stream_mask & (1u << i))) continue;
        const uint32_t vb = blk.streams[i].vb;
        if (!vb || safe_mem::CanAccess(vb, 16u)) {
            CallStdcall3(ctx, base, sub_00415130, i, vb, blk.streams[i].stride);
        }
    }

    // 2. Direct host state + guest mirror updates under d3d::Lock().
    {
        std::lock_guard<std::recursive_mutex> lock(d3d::Lock());
        gpu::State& s = d3d::State();

        if (blk.has_palettes) {
            s.palettes = blk.palettes;
            const uint32_t pDev = safe_mem::Read32(render_accuracy::addr::kD3dDevicePtr, 0u);
            if (pDev && safe_mem::CanAccess(pDev + render_accuracy::addr::kDevPalettes, 16u)) {
                for (uint32_t i = 0; i < 4u; ++i) {
                    kernel::W32(pDev + render_accuracy::addr::kDevPalettes + i * 4u, blk.palettes[i]);
                }
            }
        }

        // Pixel shader MUST be applied before PS render states and PS constants.
        if (blk.has_ps) {
            d3d_sync::LoadPixelShaderIntoState(s, blk.ps);
        }

        if (blk.has_vs) {
            s.vs_handle = blk.vs_handle;
            s.vs = blk.vs;
        }

        for (uint32_t i = 0; i < 192u; ++i) {
            if (blk.vsc_mask[i]) s.vsc[i] = blk.vsc[i];
        }

        for (uint32_t r = 0; r < gpu::kRsMax; ++r) {
            if (!blk.rs_mask[r]) continue;
            if (!s.ps && (r <= 56u || r == gpu::kRsPsTextureModes)) continue;
            d3d_sync::ApplyRenderState(s, r, blk.rs_vals[r]);
        }

        for (uint32_t i = 0; i < 16u; ++i) {
            if (!(blk.psc_mask & (1u << i))) continue;
            s.psc[i] = blk.psc[i];
            if (blk.apply_psc_mapping && s.ps) {
                gpu::ApplyPixelConstants(s, i, 1u);
            }
        }

        for (uint32_t st = 0; st < 4u; ++st) {
            for (uint32_t slot = 0; slot < gpu::kTssCount; ++slot) {
                if (blk.tss_mask[st][slot]) {
                    d3d_sync::ApplyTextureStageState(s, st, slot, blk.tss_vals[st][slot]);
                }
            }
        }

        const uint32_t pDev = safe_mem::Read32(render_accuracy::addr::kD3dDevicePtr, 0u);
        for (uint32_t t = 0; t < 10u; ++t) {
            if (!(blk.transform_mask & (1u << t))) continue;
            s.transforms[d3d_sync::D3d8TransformType(t)] = blk.transforms[t];
            if (pDev && safe_mem::CanAccess(pDev + d3d_sync::kDevTransforms + t * 64u, 64u)) {
                std::memcpy(kernel::HostPtr(pDev + d3d_sync::kDevTransforms + t * 64u),
                            blk.transforms[t].data(), 64u);
            }
        }

        if (blk.has_material) s.material = blk.material;
        for (uint32_t i = 0; i < 8u; ++i) {
            if (blk.light_mask & (1u << i)) s.lights[i] = blk.lights[i];
            if (blk.light_enable_mask & (1u << i)) {
                if (blk.light_enable & (1u << i)) s.light_enable |= (1u << i);
                else s.light_enable &= ~(1u << i);
            }
        }

        d3d_sync::MirrorHostStateToGuest(s);
    }

    // 3. Viewport via D3DDevice_SetViewport so viewport constants (c-38/c-37) refresh cleanly.
    if (blk.has_viewport) {
        const uint32_t saved_esp = ctx.esp;
        ctx.esp -= 24u;
        const uint32_t vp_va = ctx.esp;
        if (safe_mem::CanAccess(vp_va, 24u)) {
            kernel::W32(vp_va + 0u, blk.viewport.x);
            kernel::W32(vp_va + 4u, blk.viewport.y);
            kernel::W32(vp_va + 8u, blk.viewport.w);
            kernel::W32(vp_va + 12u, blk.viewport.h);
            std::memcpy(kernel::HostPtr(vp_va + 16u), &blk.viewport.min_z, 4u);
            std::memcpy(kernel::HostPtr(vp_va + 20u), &blk.viewport.max_z, 4u);
            CallStdcall1(ctx, base, sub_00411D80, vp_va);
        }
        ctx.esp = saved_esp;
    }

    render_accuracy::InvalidateMc3dStateShadows();
}

void RestoreFullRecordedState(X86Context& ctx, uint8_t* base, const RecordingContext& rec) {
    // Restore resources through HLE so CDevice refcounts stay balanced.
    for (uint32_t i = 0; i < 4u; ++i) {
        const uint32_t tex = rec.saved_state.textures[i];
        if (!tex || safe_mem::CanAccess(tex, 20u)) CallStdcall2(ctx, base, sub_004123D0, i, tex);
    }
    if (!rec.saved_state.index_buffer || safe_mem::CanAccess(rec.saved_state.index_buffer, 16u)) {
        CallStdcall2(ctx, base, sub_00412610, rec.saved_state.index_buffer, rec.saved_state.base_vertex);
    }
    for (uint32_t i = 0; i < 16u; ++i) {
        const uint32_t vb = rec.saved_state.streams[i].vb;
        if (!vb || safe_mem::CanAccess(vb, 16u)) {
            CallStdcall3(ctx, base, sub_00415130, i, vb, rec.saved_state.streams[i].stride);
        }
    }

    {
        std::lock_guard<std::recursive_mutex> lock(d3d::Lock());
        gpu::State& s = d3d::State();
        s = rec.saved_state;
        if (safe_mem::CanAccess(render_accuracy::addr::kD3dRenderState, 166u * 4u)) {
            std::memcpy(kernel::HostPtr(render_accuracy::addr::kD3dRenderState),
                        rec.saved_guest_rs.data(), 166u * 4u);
        }
        if (safe_mem::CanAccess(render_accuracy::addr::kD3dDeferredTss, 128u * 4u)) {
            std::memcpy(kernel::HostPtr(render_accuracy::addr::kD3dDeferredTss),
                        rec.saved_guest_tss.data(), 128u * 4u);
        }
        const uint32_t pDev = safe_mem::Read32(render_accuracy::addr::kD3dDevicePtr, 0u);
        if (pDev) {
            for (uint32_t t = 0; t < 10u; ++t) {
                if (safe_mem::CanAccess(pDev + d3d_sync::kDevTransforms + t * 64u, 64u)) {
                    std::memcpy(kernel::HostPtr(pDev + d3d_sync::kDevTransforms + t * 64u),
                                s.transforms[d3d_sync::D3d8TransformType(t)].data(), 64u);
                }
            }
            if (safe_mem::CanAccess(pDev + d3d_sync::kDevViewport, 24u)) {
                kernel::W32(pDev + d3d_sync::kDevViewport + 0u, s.viewport.x);
                kernel::W32(pDev + d3d_sync::kDevViewport + 4u, s.viewport.y);
                kernel::W32(pDev + d3d_sync::kDevViewport + 8u, s.viewport.w);
                kernel::W32(pDev + d3d_sync::kDevViewport + 12u, s.viewport.h);
                std::memcpy(kernel::HostPtr(pDev + d3d_sync::kDevViewport + 16u), &s.viewport.min_z, 4u);
                std::memcpy(kernel::HostPtr(pDev + d3d_sync::kDevViewport + 20u), &s.viewport.max_z, 4u);
            }
        }
        d3d_sync::MirrorHostStateToGuest(s);
    }
    render_accuracy::InvalidateMc3dStateShadows();
}

void PopulateMaskFromRecordingTable(HostStateBlock& blk, const RecordingContext& rec,
                                    const gpu::State& after) {
    uint8_t tbl[kRecTableBytes]{};
    if (safe_mem::CanAccess(kRecTableBase, kRecTableBytes)) {
        std::memcpy(tbl, kernel::HostPtr(kRecTableBase), kRecTableBytes);
    }

    for (uint32_t i = 0; i < 4u; ++i) {
        if (tbl[kOffTextures + i] || after.textures[i] != rec.saved_state.textures[i]) {
            blk.tex_mask |= static_cast<uint8_t>(1u << i);
        }
    }
    if (tbl[kOffPs] || after.ps != rec.saved_state.ps) blk.has_ps = true;
    if (tbl[kOffVs] || after.vs_handle != rec.saved_state.vs_handle) blk.has_vs = true;
    if (tbl[kOffIndices] || after.index_buffer != rec.saved_state.index_buffer ||
        after.base_vertex != rec.saved_state.base_vertex) {
        blk.has_indices = true;
    }
    for (uint32_t i = 0; i < 16u; ++i) {
        if (tbl[kOffStreams + i] || after.streams[i].vb != rec.saved_state.streams[i].vb ||
            after.streams[i].stride != rec.saved_state.streams[i].stride) {
            blk.stream_mask |= static_cast<uint16_t>(1u << i);
        }
    }
    for (uint32_t i = 0; i < 16u; ++i) {
        if (tbl[kOffPsc + i] || after.psc[i] != rec.saved_state.psc[i]) {
            blk.psc_mask |= static_cast<uint16_t>(1u << i);
        }
    }
    for (uint32_t i = 0; i < 192u; ++i) {
        if (tbl[kOffVsc + i] || after.vsc[i] != rec.saved_state.vsc[i]) {
            blk.vsc_mask[i] = 1u;
        }
    }
    for (uint32_t r = 0; r < gpu::kRsMax; ++r) {
        if (tbl[kOffRs + r] ||
            d3d_sync::ReadRenderState(after, r) != d3d_sync::ReadRenderState(rec.saved_state, r)) {
            blk.rs_mask[r] = 1u;
        }
    }
    for (uint32_t slot = 0; slot < gpu::kTssCount; ++slot) {
        for (uint32_t st = 0; st < 4u; ++st) {
            if (tbl[kOffTss + slot * 4u + st] ||
                after.tss[st][slot] != rec.saved_state.tss[st][slot]) {
                blk.tss_mask[st][slot] = 1u;
            }
        }
    }
    for (uint32_t t = 0; t < 10u; ++t) {
        const uint32_t dt = d3d_sync::D3d8TransformType(t);
        if (tbl[kOffTransforms + t] || after.transforms[dt] != rec.saved_state.transforms[dt]) {
            blk.transform_mask |= static_cast<uint16_t>(1u << t);
        }
    }
    if (tbl[kOffViewport]) blk.has_viewport = true;
    if (tbl[kOffMaterial] || after.material != rec.saved_state.material) blk.has_material = true;

    blk.light_mask = rec.touched_lights;
    blk.light_enable_mask = rec.touched_light_enables;
    for (uint32_t i = 0; i < 8u; ++i) {
        if (after.lights[i] != rec.saved_state.lights[i]) {
            blk.light_mask |= static_cast<uint8_t>(1u << i);
        }
        if (((after.light_enable ^ rec.saved_state.light_enable) >> i) & 1u) {
            blk.light_enable_mask |= static_cast<uint8_t>(1u << i);
        }
    }
    blk.apply_psc_mapping = true;
}

void PopulateMaskForType(HostStateBlock& blk, uint32_t type) {
    blk.apply_psc_mapping = false;
    if (type == 1u) {  // D3DSBT_ALL
        blk.tex_mask = 0x0Fu;
        blk.has_palettes = true;
        blk.has_ps = true;
        blk.has_vs = true;
        blk.has_indices = true;
        blk.stream_mask = 0xFFFFu;
        blk.psc_mask = 0xFFFFu;
        blk.vsc_mask.fill(1u);
        blk.rs_mask.fill(1u);
        for (auto& st : blk.tss_mask) st.fill(1u);
        blk.transform_mask = 0x03FFu;
        blk.has_viewport = true;
        blk.has_material = true;
        blk.light_mask = 0xFFu;
        blk.light_enable_mask = 0xFFu;
    } else if (type == 2u) {  // D3DSBT_PIXELSTATE
        blk.has_ps = true;
        blk.psc_mask = 0xFFFFu;
        for (uint8_t r : kPixelStateRs) blk.rs_mask[r] = 1u;
        for (uint32_t st = 0; st < 4u; ++st) {
            for (uint8_t slot : kPixelStateTss) blk.tss_mask[st][slot] = 1u;
        }
    } else {  // D3DSBT_VERTEXSTATE (3)
        blk.has_vs = true;
        blk.vsc_mask.fill(1u);
        for (uint8_t r : kVertexStateRs) blk.rs_mask[r] = 1u;
        for (uint32_t st = 0; st < 4u; ++st) {
            for (uint8_t slot : kVertexStateTss) blk.tss_mask[st][slot] = 1u;
        }
        blk.light_mask = 0xFFu;
        blk.light_enable_mask = 0xFFu;
    }
}

uint32_t RegisterBlock(std::shared_ptr<HostStateBlock> blk) {
    std::lock_guard<std::mutex> lk(g_sb_mu);
    const uint32_t token = g_next_token++;
    g_blocks[token] = std::move(blk);
    return token;
}

}  // namespace

bool IsRecording() {
    return g_recording_flag.load(std::memory_order_acquire);
}

void RecordLightTouch(uint32_t light_idx, bool set_light, bool set_enable) {
    if (light_idx >= 8u || !IsRecording()) return;
    std::lock_guard<std::mutex> lk(g_sb_mu);
    if (!g_rec.active) return;
    if (set_light) g_rec.touched_lights |= static_cast<uint8_t>(1u << light_idx);
    if (set_enable) g_rec.touched_light_enables |= static_cast<uint8_t>(1u << light_idx);
}

uint32_t Begin(X86Context& /*ctx*/, uint8_t* /*base*/) {
    std::lock_guard<std::mutex> lk(g_sb_mu);
    if (!g_rec.active) {
        std::lock_guard<std::recursive_mutex> lock(d3d::Lock());
        gpu::State& s = d3d::State();
        d3d_sync::PullGuestDeferredToHost(s);
        g_rec.saved_state = s;
        if (safe_mem::CanAccess(render_accuracy::addr::kD3dRenderState, 166u * 4u)) {
            std::memcpy(g_rec.saved_guest_rs.data(),
                        kernel::HostPtr(render_accuracy::addr::kD3dRenderState), 166u * 4u);
        }
        if (safe_mem::CanAccess(render_accuracy::addr::kD3dDeferredTss, 128u * 4u)) {
            std::memcpy(g_rec.saved_guest_tss.data(),
                        kernel::HostPtr(render_accuracy::addr::kD3dDeferredTss), 128u * 4u);
        }
    }
    g_rec.touched_lights = 0;
    g_rec.touched_light_enables = 0;
    g_rec.active = true;
    g_recording_flag.store(true, std::memory_order_release);

    if (safe_mem::CanAccess(kRecTableBase, kRecTableBytes)) {
        std::memset(kernel::HostPtr(kRecTableBase), 0, kRecTableBytes);
    }
    const uint32_t pDev = safe_mem::Read32(render_accuracy::addr::kD3dDevicePtr, 0u);
    if (pDev && safe_mem::CanAccess(pDev + render_accuracy::addr::kDevFlags, 4u)) {
        kernel::W32(pDev + render_accuracy::addr::kDevFlags,
                    kernel::R32(pDev + render_accuracy::addr::kDevFlags) | 0x20u);
    }
    return 0u;
}

uint32_t End(X86Context& ctx, uint8_t* base, uint32_t out_token_va) {
    RecordingContext rec_copy;
    {
        std::lock_guard<std::mutex> lk(g_sb_mu);
        rec_copy = g_rec;
        g_rec.active = false;
        g_recording_flag.store(false, std::memory_order_release);
    }

    const uint32_t pDev = safe_mem::Read32(render_accuracy::addr::kD3dDevicePtr, 0u);
    if (pDev && safe_mem::CanAccess(pDev + render_accuracy::addr::kDevFlags, 4u)) {
        kernel::W32(pDev + render_accuracy::addr::kDevFlags,
                    kernel::R32(pDev + render_accuracy::addr::kDevFlags) & ~0x20u);
    }

    gpu::State after;
    {
        std::lock_guard<std::recursive_mutex> lock(d3d::Lock());
        d3d_sync::PullGuestDeferredToHost(d3d::State());
        after = d3d::State();
    }

    auto blk = std::make_shared<HostStateBlock>();
    PopulateMaskFromRecordingTable(*blk, rec_copy, after);
    CaptureValuesIntoBlock(ctx, base, *blk, false);

    // Restore pre-BeginStateBlock live state so recording never clobbers active device state.
    if (rec_copy.active) {
        RestoreFullRecordedState(ctx, base, rec_copy);
    }

    const uint32_t token = RegisterBlock(blk);
    if (out_token_va && safe_mem::CanAccess(out_token_va, 4u)) {
        kernel::W32(out_token_va, token);
    }
    const uint64_t n = ++g_created_count;
    if (n <= 12u) {
        LOGR("stateblock_hle: EndStateBlock tok=%08X ps=%d(%08X) vs=%d(%08X) tex=%X psc=%X",
             token, blk->has_ps ? 1 : 0, blk->ps ? blk->ps->handle : 0u,
             blk->has_vs ? 1 : 0, blk->vs_handle, blk->tex_mask, blk->psc_mask);
    }
    return 0u;
}

uint32_t Create(X86Context& ctx, uint8_t* base, uint32_t type, uint32_t out_token_va) {
    auto blk = std::make_shared<HostStateBlock>();
    PopulateMaskForType(*blk, type);
    CaptureValuesIntoBlock(ctx, base, *blk, false);
    const uint32_t token = RegisterBlock(blk);
    if (out_token_va && safe_mem::CanAccess(out_token_va, 4u)) {
        kernel::W32(out_token_va, token);
    }
    const uint64_t n = ++g_created_count;
    if (n <= 12u) {
        LOGR("stateblock_hle: CreateStateBlock(type=%u) tok=%08X ps=%08X vs=%08X",
             type, token, blk->ps ? blk->ps->handle : 0u, blk->vs_handle);
    }
    return 0u;
}

uint32_t Capture(X86Context& ctx, uint8_t* base, uint32_t token) {
    if (!token) return 0u;
    std::shared_ptr<HostStateBlock> blk;
    {
        std::lock_guard<std::mutex> lk(g_sb_mu);
        auto it = g_blocks.find(token);
        if (it == g_blocks.end()) return 0u;
        blk = it->second;
    }
    CaptureValuesIntoBlock(ctx, base, *blk, true);
    return 0u;
}

uint32_t Apply(X86Context& ctx, uint8_t* base, uint32_t token) {
    if (!token) return 0u;
    std::shared_ptr<HostStateBlock> blk;
    {
        std::lock_guard<std::mutex> lk(g_sb_mu);
        auto it = g_blocks.find(token);
        if (it == g_blocks.end()) return 0u;
        blk = it->second;
    }
    ApplyBlockToDevice(ctx, base, *blk);
    const uint64_t n = ++g_applied_count;
    if (n <= 8u || (n % 5000u) == 0u) {
        LOGR("stateblock_hle: ApplyStateBlock #%llu tok=%08X ps=%d(%08X) vs=%d(%08X)",
             static_cast<unsigned long long>(n), token,
             blk->has_ps ? 1 : 0, blk->ps ? blk->ps->handle : 0u,
             blk->has_vs ? 1 : 0, blk->vs_handle);
    }
    return 0u;
}

uint32_t Delete(X86Context& ctx, uint8_t* base, uint32_t token) {
    if (!token) return 0u;
    std::shared_ptr<HostStateBlock> blk;
    {
        std::lock_guard<std::mutex> lk(g_sb_mu);
        auto it = g_blocks.find(token);
        if (it == g_blocks.end()) return 0u;
        blk = std::move(it->second);
        g_blocks.erase(it);
    }
    if (blk) ReleaseBlockResources(ctx, base, *blk);
    return 0u;
}

}  // namespace stateblock
}  // namespace game
