#include "game/render_accuracy.h"
#include "game/render_accuracy_internal.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/log.h"
#include "cpu/x86_ops.h"
#include "gpu/shaders.h"
#include "gpu/state.h"
#include "gpu/texture.h"
#include "gpu/vertex.h"
#include "image_symbols.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "xdk/d3d8.h"
#include "xdk/hle.h"

extern "C" void sub_004168B0(X86Context& ctx, uint8_t* base);  // D3D_KickOffAndWaitForIdle
extern "C" void sub_00417570(X86Context& ctx, uint8_t* base);  // D3DDevice_DrawVerticesUP

namespace game {
namespace render_accuracy {

namespace {

constexpr uint32_t kPushGuardSentinel = 0xFEEDBABEu;
constexpr uint32_t kDeferredTssSlots = 22;  // slots 0..21 synced by d3d8.cpp SyncDeferred()
constexpr uint32_t kMaxDiagFf   = 48;
constexpr uint32_t kMaxDiagProg = 64;

// D3D8 TSS enums / values matching Xbox XDK 5849 & shaders.cpp.
constexpr uint32_t kTopDisable     = 1;
constexpr uint32_t kTopSelectArg1  = 2;
constexpr uint32_t kTopSelectArg2  = 3;
constexpr uint32_t kTaMask         = 0x0Fu;
constexpr uint32_t kTaModMask      = 0xF0u;
constexpr uint32_t kTaDiffuse      = 0u;
constexpr uint32_t kTaCurrent      = 1u;
constexpr uint32_t kTaTexture      = 2u;

// Runtime state across draws.
std::once_flag g_init_once;
std::atomic<bool> g_initialized{false};
std::atomic<uint64_t> g_apply_calls{0};
std::atomic<uint64_t> g_swap_count{0};
std::atomic<uint64_t> g_ff_repairs{0};
std::atomic<uint64_t> g_ps_mode_repairs{0};
std::atomic<uint64_t> g_tex_recoveries{0};
std::atomic<uint64_t> g_pal_recoveries{0};
std::atomic<uint64_t> g_push_flushes{0};

std::mutex g_cache_mu;
uint32_t g_last_dev_tex[4] = {};
uint32_t g_last_dev_pal[4] = {};
std::unordered_map<uint32_t, uint32_t> g_ps_orig_def54;

// Thread-safe clone cache keyed by (original_ps_handle, resolved_tex_modes),
// avoiding in-place mutation of PixelShader objects in flight on render_thread.
struct PsCloneKey {
    uint32_t handle;
    uint32_t tex_modes;
    bool operator==(const PsCloneKey& o) const { return handle == o.handle && tex_modes == o.tex_modes; }
};
struct PsCloneHash {
    size_t operator()(const PsCloneKey& k) const {
        return (size_t(k.handle) * 1315423911u) ^ (size_t(k.tex_modes) << 1);
    }
};
std::unordered_map<PsCloneKey, std::shared_ptr<gpu::PixelShader>, PsCloneHash> g_ps_clones;

// Bounded per-shape diagnostic deduplication (tracked separately so [FF] menus never crowd out Garage [PROG] shaders).
std::unordered_set<uint64_t> g_logged_ff;
std::unordered_set<uint64_t> g_logged_prog;
uint32_t g_last_s_tex[4] = {};

// Contiguous scratch ring for HLE DrawIndexedVerticesUP execution.
constexpr uint32_t kDiupRingBytes = 4u << 20;
constexpr uint32_t kDiupHeaderCount = 64;
std::mutex g_diup_mu;
uint32_t g_diup_ring = 0;
uint32_t g_diup_pos = 0;
uint32_t g_diup_hdr_idx = 0;
uint32_t g_diup_headers[kDiupHeaderCount] = {};

thread_local uint32_t tl_push_begin = 0;
thread_local uint32_t tl_push_max_dwords = 0;
thread_local uint32_t tl_push_vs_handle = 0;
thread_local std::shared_ptr<gpu::VertexShader> tl_push_vs;

inline bool UsesTextureArg(uint32_t arg) {
    return (arg & kTaMask) == kTaTexture;
}

inline uint32_t ReplaceTextureArg(uint32_t arg, uint32_t replacement) {
    return UsesTextureArg(arg) ? ((arg & kTaModMask) | replacement) : arg;
}

void WriteTss(gpu::State& s, uint32_t st, uint32_t slot, uint32_t val) {
    s.tss[st][slot] = val;
    if (slot < 32) {
        SafeW32(addr::kD3dDeferredTss + st * 0x80u + slot * 4u, val);
    }
}

void WriteRs(gpu::State& s, uint32_t rs, uint32_t val) {
    if (rs < gpu::kRsMax) {
        s.rs[rs] = val;
        SafeW32(addr::kD3dRenderState + rs * 4u, val);
    }
}

bool TryDescribeTexture(uint32_t tex_va, gpu::TextureDesc& out) {
    if (!tex_va || !CanAccessGuest(tex_va, 20)) return false;
    return gpu::Describe(tex_va, out) && out.data != 0;
}

bool IsP8Format(const gpu::TextureDesc& d) {
    if (d.format == 0x0Bu) return true;
    const char* name = gpu::Format(d.format).name;
    return name && std::strstr(name, "P8") != nullptr;
}

// Pulls latest deferred RS/TSS from guest RAM so fixes operate on post-game state.
void PullDeferredFromGuest(gpu::State& s) {
    for (uint32_t st = 0; st < 4; ++st) {
        for (uint32_t slot = 0; slot < kDeferredTssSlots; ++slot) {
            uint32_t ea = addr::kD3dDeferredTss + st * 0x80u + slot * 4u;
            if (CanAccessGuest(ea, 4)) s.tss[st][slot] = kernel::R32(ea);
        }
        uint32_t tci_ea = addr::kD3dDeferredTss + st * 0x80u + gpu::kTssTexCoordIndex * 4u;
        if (CanAccessGuest(tci_ea, 4)) s.tss[st][gpu::kTssTexCoordIndex] = kernel::R32(tci_ea);
    }
    for (uint32_t rs = gpu::kRsDeferredFirst; rs <= 135; ++rs) {
        uint32_t ea = addr::kD3dRenderState + rs * 4u;
        if (CanAccessGuest(ea, 4)) s.rs[rs] = kernel::R32(ea);
    }
}

void SyncStageTexturesAndPalettes(gpu::State& s, std::array<gpu::TextureDesc, 4>& tex_descs,
                                  std::array<bool, 4>& tex_ok) {
    const uint32_t pDev = SafeR32(addr::kD3dDevicePtr);
    for (uint32_t st = 0; st < 4; ++st) {
        const uint32_t dev_tex = pDev ? SafeR32(pDev + addr::kDevTextures + st * 4u) : 0;
        const uint32_t dev_pal = pDev ? SafeR32(pDev + addr::kDevPalettes + st * 4u) : 0;
        const uint32_t mc_pal  = SafeR32(addr::kMcPalettesShadow + st * 4u);

        {
            std::lock_guard<std::mutex> lk(g_cache_mu);
            const bool dev_changed = (dev_tex != g_last_dev_tex[st]);
            const bool d3d_stale = (s.textures[st] != dev_tex) && (s.textures[st] == g_last_s_tex[st] || s.textures[st] == 0);
            if ((dev_changed && d3d_stale && dev_tex != s.textures[st]) || (s.textures[st] == 0 && dev_tex != 0)) {
                s.textures[st] = dev_tex;
                ++g_tex_recoveries;
            }
            g_last_dev_tex[st] = dev_tex;
            g_last_s_tex[st] = s.textures[st];

            if ((dev_pal != g_last_dev_pal[st] && dev_pal != s.palettes[st]) || (s.palettes[st] == 0 && dev_pal != 0)) {
                s.palettes[st] = dev_pal;
                ++g_pal_recoveries;
            }
            g_last_dev_pal[st] = dev_pal;
        }

        tex_ok[st] = TryDescribeTexture(s.textures[st], tex_descs[st]);
        if (!tex_ok[st] && s.textures[st] != 0) {
            LOG_FIRST(16, "render_accuracy: Describe failed st=%u tex=%08X common=%08X data=%08X fmt=%08X size=%08X",
                      st, s.textures[st], SafeR32(s.textures[st]), SafeR32(s.textures[st] + 4),
                      SafeR32(s.textures[st] + 12), SafeR32(s.textures[st] + 16));
            // Do not leave a broken pointer masquerading as a valid texture.
            if (dev_tex != s.textures[st] && TryDescribeTexture(dev_tex, tex_descs[st])) {
                s.textures[st] = dev_tex;
                tex_ok[st] = true;
                ++g_tex_recoveries;
            }
        }

        if (tex_ok[st] && IsP8Format(tex_descs[st]) && s.palettes[st] == 0) {
            if (dev_pal && CanAccessGuest(dev_pal, 8)) {
                s.palettes[st] = dev_pal;
                ++g_pal_recoveries;
            } else if (mc_pal && CanAccessGuest(mc_pal, 8)) {
                s.palettes[st] = mc_pal;
                ++g_pal_recoveries;
            }
        }
    }
}

uint32_t ScanCombinerTexRegisterMask(const uint32_t* def) {
    uint32_t num_stages = std::clamp<uint32_t>(def[53] & 0x0Fu, 1u, 8u);
    uint32_t mask = 0;
    auto check_word = [&](uint32_t w, int count) {
        for (int b = 0; b < count; ++b) {
            uint32_t reg = (w >> (b * 8)) & 0x0Fu;
            if (reg >= 0x8u && reg <= 0xBu) mask |= 1u << (reg - 0x8u);
        }
    };
    for (uint32_t i = 0; i < num_stages; ++i) {
        check_word(def[0 + i], 4);   // AlphaInputs
        check_word(def[34 + i], 4);  // RgbInputs
    }
    check_word(def[8], 4);  // FinalAbcd
    check_word(def[9], 3);  // FinalEfgh (E, F, G)
    return mask;
}

uint32_t InferTexModeForDesc(const gpu::TextureDesc& d) {
    if (d.cube) return 3u;       // PS_TEXTUREMODES_CUBEMAP
    if (d.depth > 1) return 2u;  // PS_TEXTUREMODES_PROJECT3D
    return 1u;                   // PS_TEXTUREMODES_PROJECT2D
}

void SyncProgrammablePixelShader(gpu::State& s, const std::array<gpu::TextureDesc, 4>& tex_descs,
                                 const std::array<bool, 4>& tex_ok) {
    const uint32_t pDev = SafeR32(addr::kD3dDevicePtr);
    const uint32_t rs_guest = SafeR32(addr::kD3dPsTexModesRs);
    const uint32_t dev_modes = pDev ? SafeR32(pDev + addr::kDevPsTexModes) : 0;
    const uint32_t rs_state = s.rs[gpu::kRsPsTextureModes];

    uint32_t orig_def54 = s.ps->def[54];
    {
        std::lock_guard<std::mutex> lk(g_cache_mu);
        auto [it, inserted] = g_ps_orig_def54.emplace(s.ps->handle, s.ps->def[54]);
        orig_def54 = it->second;
    }

    uint32_t eff_modes = orig_def54;
    // Detect explicit SetRenderState(D3DRS_PSTEXTUREMODES) overriding shader creation modes.
    if (rs_state != 0 && rs_state != orig_def54) {
        eff_modes = rs_state;
    } else if (rs_guest != 0 && rs_guest != orig_def54) {
        eff_modes = rs_guest;
    } else if (eff_modes == 0) {
        if (rs_state != 0) eff_modes = rs_state;
        else if (rs_guest != 0) eff_modes = rs_guest;
        else if (dev_modes != 0) eff_modes = dev_modes;
    }

    const uint32_t read_mask = ScanCombinerTexRegisterMask(s.ps->def);
    for (uint32_t st = 0; st < 4; ++st) {
        uint32_t m = (eff_modes >> (st * 5)) & 0x1Fu;
        if (m == 0) {
            uint32_t rs_m = (rs_state >> (st * 5)) & 0x1Fu;
            if (!rs_m) rs_m = (rs_guest >> (st * 5)) & 0x1Fu;
            if (rs_m != 0) {
                m = rs_m;
                eff_modes |= (m << (st * 5));
            } else if ((read_mask & (1u << st)) && tex_ok[st]) {
                m = InferTexModeForDesc(tex_descs[st]);
                eff_modes |= (m << (st * 5));
            }
        }
    }

    WriteRs(s, gpu::kRsPsTextureModes, eff_modes);

    if (s.ps->def[54] != eff_modes) {
        ++g_ps_mode_repairs;
        std::lock_guard<std::mutex> lk(g_cache_mu);
        PsCloneKey key{s.ps->handle, eff_modes};
        auto it = g_ps_clones.find(key);
        std::shared_ptr<gpu::PixelShader> cloned;
        if (it != g_ps_clones.end() &&
            std::memcmp(it->second->def, s.ps->def, 54 * sizeof(uint32_t)) == 0 &&
            it->second->def[55] == s.ps->def[55] &&
            it->second->def[56] == s.ps->def[56]) {
            cloned = it->second;
        } else {
            cloned = std::make_shared<gpu::PixelShader>(*s.ps);
            cloned->def[54] = eff_modes;
            g_ps_clones[key] = cloned;
        }
        s.ps = std::move(cloned);
    }
}

bool StageReadsTexture(const gpu::State& s, uint32_t st) {
    const uint32_t cop = s.tss[st][gpu::kTssColorOp];
    const uint32_t aop = s.tss[st][gpu::kTssAlphaOp];
    if (cop > kTopDisable) {
        if (UsesTextureArg(s.tss[st][gpu::kTssColorArg1])) return true;
        if (cop > kTopSelectArg1 && UsesTextureArg(s.tss[st][gpu::kTssColorArg2])) return true;
        if (UsesTextureArg(s.tss[st][gpu::kTssColorArg0])) return true;
    }
    if (aop > kTopDisable) {
        if (UsesTextureArg(s.tss[st][gpu::kTssAlphaArg1])) return true;
        if (aop > kTopSelectArg1 && UsesTextureArg(s.tss[st][gpu::kTssAlphaArg2])) return true;
        if (UsesTextureArg(s.tss[st][gpu::kTssAlphaArg0])) return true;
    }
    return false;
}

// Rewrites a color or alpha op slot when TEXTURE is unbound so binary blends
// (MODULATE, ADD, etc.) pass through the non-texture operand cleanly instead
// of squaring CURRENT*CURRENT or sampling constant white (1,1,1,1).
bool RepairUnboundStageOp(gpu::State& s, uint32_t st, uint32_t op_slot,
                          uint32_t a1_slot, uint32_t a2_slot, uint32_t a0_slot,
                          uint32_t tex_fallback) {
    const uint32_t op = s.tss[st][op_slot];
    if (op <= kTopDisable) return false;

    const uint32_t a1 = s.tss[st][a1_slot];
    const uint32_t a2 = s.tss[st][a2_slot];
    const uint32_t a0 = s.tss[st][a0_slot];
    bool changed = false;

    if (op == kTopSelectArg1) {
        if (UsesTextureArg(a1)) {
            WriteTss(s, st, a1_slot, ReplaceTextureArg(a1, tex_fallback));
            changed = true;
        }
    } else if (op == kTopSelectArg2) {
        if (UsesTextureArg(a2)) {
            WriteTss(s, st, a2_slot, ReplaceTextureArg(a2, tex_fallback));
            changed = true;
        }
    } else {
        const bool t1 = UsesTextureArg(a1);
        const bool t2 = UsesTextureArg(a2);
        if (t1 && !t2) {
            WriteTss(s, st, op_slot, kTopSelectArg2);
            changed = true;
        } else if (t2 && !t1) {
            WriteTss(s, st, op_slot, kTopSelectArg1);
            changed = true;
        } else if (t1 && t2) {
            WriteTss(s, st, op_slot, kTopSelectArg1);
            WriteTss(s, st, a1_slot, ReplaceTextureArg(a1, tex_fallback));
            changed = true;
        }
        if (UsesTextureArg(a0)) {
            WriteTss(s, st, a0_slot, ReplaceTextureArg(a0, tex_fallback));
            changed = true;
        }
    }
    return changed;
}

bool IsStagePassThrough(const gpu::State& s, uint32_t st) {
    auto is_cur_pass = [&](uint32_t op, uint32_t a1, uint32_t a2) {
        if (op == kTopDisable) return true;
        if (op == kTopSelectArg1 && a1 == kTaCurrent) return true;
        if (op == kTopSelectArg2 && a2 == kTaCurrent) return true;
        return false;
    };
    return is_cur_pass(s.tss[st][gpu::kTssColorOp], s.tss[st][gpu::kTssColorArg1], s.tss[st][gpu::kTssColorArg2]) &&
           is_cur_pass(s.tss[st][gpu::kTssAlphaOp], s.tss[st][gpu::kTssAlphaArg1], s.tss[st][gpu::kTssAlphaArg2]);
}

// Prevents fixed-function [FF] unbound texture stages (kTexNone -> CONST(1,1,1,1)) from
// overwriting textured Stage 0 output or diffuse vertex shading with solid white or cur*cur.
void SyncFixedFunctionStages(gpu::State& s, const std::array<bool, 4>& tex_ok) {
    if (!tex_ok[0]) {
        if (RepairUnboundStageOp(s, 0, gpu::kTssColorOp, gpu::kTssColorArg1, gpu::kTssColorArg2,
                                 gpu::kTssColorArg0, kTaDiffuse)) {
            ++g_ff_repairs;
        }
        if (RepairUnboundStageOp(s, 0, gpu::kTssAlphaOp, gpu::kTssAlphaArg1, gpu::kTssAlphaArg2,
                                 gpu::kTssAlphaArg0, kTaDiffuse)) {
            ++g_ff_repairs;
        }
    }

    uint32_t active_stages = 1;
    for (uint32_t st = 1; st < 4; ++st) {
        if (s.tss[st][gpu::kTssColorOp] <= kTopDisable) break;
        active_stages = st + 1;
        if (!tex_ok[st]) {
            if (RepairUnboundStageOp(s, st, gpu::kTssColorOp, gpu::kTssColorArg1, gpu::kTssColorArg2,
                                     gpu::kTssColorArg0, kTaCurrent)) {
                ++g_ff_repairs;
            }
            if (RepairUnboundStageOp(s, st, gpu::kTssAlphaOp, gpu::kTssAlphaArg1, gpu::kTssAlphaArg2,
                                     gpu::kTssAlphaArg0, kTaCurrent)) {
                ++g_ff_repairs;
            }
        }
    }

    for (uint32_t st = 1; st < active_stages; ++st) {
        bool rest_have_work = false;
        for (uint32_t k = st; k < active_stages; ++k) {
            if (!IsStagePassThrough(s, k)) {
                rest_have_work = true;
                break;
            }
        }
        if (!rest_have_work) {
            WriteTss(s, st, gpu::kTssColorOp, kTopDisable);
            WriteTss(s, st, gpu::kTssAlphaOp, kTopDisable);
            ++g_ff_repairs;
            break;
        }
    }
}

void LogUniqueDrawState(const gpu::State& s, const std::array<gpu::TextureDesc, 4>& tex_descs,
                        const std::array<bool, 4>& tex_ok) {
    if (g_swap_count.load(std::memory_order_relaxed) < 5) return;

    if (s.ps) {
        const uint32_t read_mask = ScanCombinerTexRegisterMask(s.ps->def);
        uint64_t sig = (uint64_t(s.ps->handle) << 32) ^ s.ps->def[54] ^ (uint64_t(read_mask) << 20);
        for (int i = 0; i < 4; ++i) {
            sig ^= (uint64_t(tex_ok[i] ? tex_descs[i].format : 0xFFu) << (24 + i * 8));
        }
        std::lock_guard<std::mutex> lk(g_cache_mu);
        if (g_logged_prog.size() >= kMaxDiagProg || !g_logged_prog.insert(sig).second) return;
        LOGR("render_accuracy[PROG] ps=%08X modes=%05X readMask=%X tex=(%08X:%s,%08X:%s,%08X:%s,%08X:%s) pal=(%08X,%08X)",
             s.ps->handle, s.ps->def[54], read_mask,
             s.textures[0], tex_ok[0] ? gpu::Format(tex_descs[0].format).name : "NONE",
             s.textures[1], tex_ok[1] ? gpu::Format(tex_descs[1].format).name : "NONE",
             s.textures[2], tex_ok[2] ? gpu::Format(tex_descs[2].format).name : "NONE",
             s.textures[3], tex_ok[3] ? gpu::Format(tex_descs[3].format).name : "NONE",
             s.palettes[0], s.palettes[1]);
    } else {
        uint64_t sig = uint64_t(s.tss[0][gpu::kTssColorOp]) |
                       (uint64_t(s.tss[1][gpu::kTssColorOp]) << 8) |
                       (uint64_t(s.tss[2][gpu::kTssColorOp]) << 16) |
                       (uint64_t(s.tss[0][gpu::kTssColorArg1]) << 24);
        for (int i = 0; i < 2; ++i) {
            sig ^= (uint64_t(tex_ok[i] ? tex_descs[i].format : 0xFFu) << (32 + i * 8));
        }
        std::lock_guard<std::mutex> lk(g_cache_mu);
        if (g_logged_ff.size() >= kMaxDiagFf || !g_logged_ff.insert(sig).second) return;
        LOGR("render_accuracy[FF] cop=(%u,%u,%u) a1=(%u,%u,%u) a2=(%u,%u,%u) tex=(%08X:%s,%08X:%s) pal=%08X",
             s.tss[0][gpu::kTssColorOp], s.tss[1][gpu::kTssColorOp], s.tss[2][gpu::kTssColorOp],
             s.tss[0][gpu::kTssColorArg1], s.tss[1][gpu::kTssColorArg1], s.tss[2][gpu::kTssColorArg1],
             s.tss[0][gpu::kTssColorArg2], s.tss[1][gpu::kTssColorArg2], s.tss[2][gpu::kTssColorArg2],
             s.textures[0], tex_ok[0] ? gpu::Format(tex_descs[0].format).name : "NONE",
             s.textures[1], tex_ok[1] ? gpu::Format(tex_descs[1].format).name : "NONE",
             s.palettes[0]);
    }
}

uint32_t CurrentVertexStrideBytes(const gpu::State& s) {
    if (s.vs_handle & 1u) return s.vs ? s.vs->stream_stride[0] : 0;
    gpu::VertexShader fvf{};
    gpu::FvfInputs(s.vs_handle, fvf);
    return fvf.stream_stride[0];
}

void ApplySimpleMethodToState(gpu::State& s, uint32_t method, uint32_t val) {
    if ((method >> 2) < s.simple.size()) s.simple[method >> 2] = val;
}

uint32_t AllocDiupContiguousVertices(X86Context& ctx, uint8_t* base,
                                     const void* src, uint32_t bytes, uint32_t& out_hdr_va) {
    std::lock_guard<std::mutex> lk(g_diup_mu);
    if (!g_diup_ring) {
        g_diup_ring = mem::AllocContiguous(kDiupRingBytes, 0, 0xFFFFFFFFu, 0x1000u);
        for (uint32_t i = 0; i < kDiupHeaderCount; ++i) {
            g_diup_headers[i] = mem::AllocSystem(20, 16);
            std::memset(kernel::HostPtr(g_diup_headers[i]), 0, 20);
            // High refcount ensures SetStreamSource Release never frees permanent scratch headers.
            kernel::W32(g_diup_headers[i], 0x01400001u);
        }
        LOGR("render_accuracy: allocated DIUP ring=%08X (%u MB)", g_diup_ring, kDiupRingBytes >> 20);
    }

    const uint32_t room = (bytes + 63u) & ~63u;
    if (room > kDiupRingBytes) return 0;

    if (g_diup_pos + room > kDiupRingBytes) {
        g_diup_mu.unlock();
        X86_PUSH32(0u);
        sub_004168B0(ctx, base);  // Wait for GPU before overwriting ring head
        g_diup_mu.lock();
        g_diup_pos = 0;
    }

    const uint32_t dst_va = g_diup_ring + g_diup_pos;
    std::memcpy(kernel::HostPtr(dst_va), src, bytes);
    g_diup_pos += room;

    out_hdr_va = g_diup_headers[g_diup_hdr_idx];
    g_diup_hdr_idx = (g_diup_hdr_idx + 1u) % kDiupHeaderCount;
    kernel::W32(out_hdr_va, 0x01400001u);
    kernel::W32(out_hdr_va + 4u, mem::PhysicalFromVirtual(dst_va));
    return dst_va;
}

}  // namespace

bool CanAccessGuest(uint32_t va, uint32_t bytes) {
    if (va < 0x10000u || bytes == 0) return false;
    uint64_t end = uint64_t(va) + bytes - 1u;
    if (end > 0xFFFFFFFFull) return false;
    uint32_t p = va & ~0xFFFu;
    uint32_t last = uint32_t(end) & ~0xFFFu;
    while (true) {
        if (!mem::IsCommitted(p)) return false;
        if (p == last) break;
        p += 0x1000u;
    }
    return true;
}

uint32_t SafeR32(uint32_t va, uint32_t fallback) {
    return CanAccessGuest(va, 4) ? kernel::R32(va) : fallback;
}

uint16_t SafeR16(uint32_t va, uint16_t fallback) {
    if (!CanAccessGuest(va, 2)) return fallback;
    return *reinterpret_cast<const uint16_t*>(kernel::HostPtr(va));
}

bool SafeW32(uint32_t va, uint32_t value) {
    if (!CanAccessGuest(va, 4)) return false;
    kernel::W32(va, value);
    return true;
}

void NotifyPixelShaderSet(uint32_t /*ps_handle*/) {
}

void NotifyPaletteBound(uint32_t stage, uint32_t pal_va) {
    std::lock_guard<std::recursive_mutex> lock(d3d::Lock());
    d3d::State().palettes[stage & 3u] = pal_va;
    std::lock_guard<std::mutex> lk(g_cache_mu);
    g_last_dev_pal[stage & 3u] = pal_va;
}

void InvalidateMc3dStateShadows() {
    // Force MC3D to miss shadow comparisons and issue D3D SetTexture/SetPalette/SetPS next draw.
    for (uint32_t i = 0; i < 4; ++i) {
        SafeW32(addr::kMcTexturesShadow + i * 4u, 0xFFFFFFFFu);
        SafeW32(addr::kMcPalettesShadow + i * 4u, 0xFFFFFFFFu);
    }
    SafeW32(addr::kMcPsShadow, 0xFFFFFFFFu);
}

void RecordPushSpanBegin(uint32_t begin_va, uint32_t max_dwords) {
    if (!begin_va || !max_dwords || max_dwords > 0x20000u) return;
    if (!CanAccessGuest(begin_va, max_dwords * 4u)) return;
    for (uint32_t i = 0; i < max_dwords; ++i) {
        kernel::W32(begin_va + i * 4u, kPushGuardSentinel);
    }
    tl_push_begin = begin_va;
    tl_push_max_dwords = max_dwords;
    std::lock_guard<std::recursive_mutex> lock(d3d::Lock());
    tl_push_vs_handle = d3d::State().vs_handle;
    tl_push_vs = d3d::State().vs;
}

void FlushCapturedPushSpan(X86Context& ctx, uint8_t* base, uint32_t explicit_end, const char* tag) {
    const uint32_t begin = tl_push_begin;
    const uint32_t max_dwords = tl_push_max_dwords;
    const uint32_t saved_push_vs_handle = tl_push_vs_handle;
    std::shared_ptr<gpu::VertexShader> saved_push_vs = std::move(tl_push_vs);
    tl_push_begin = 0;
    tl_push_max_dwords = 0;
    tl_push_vs_handle = 0;
    tl_push_vs.reset();
    if (!begin || !max_dwords) return;

    const uint32_t max_end = begin + max_dwords * 4u;
    uint32_t end = (explicit_end > begin && explicit_end <= max_end) ? explicit_end : max_end;
    if (!CanAccessGuest(begin, end - begin)) return;

    constexpr uint32_t kBeginEnd = 0x17FCu;
    constexpr uint32_t kInlineArray = 0x1818u;

    uint32_t prim = 0;
    std::vector<uint32_t> vertices;
    uint32_t p = begin;
    while (p + 4 <= end) {
        const uint32_t head = kernel::R32(p);
        if (head == kPushGuardSentinel) break;
        const uint32_t method = head & 0x1FFCu;
        const uint32_t count = (head >> 18) & 0x7FFu;
        p += 4;
        if ((head & 0xA0030003u) != 0 || count * 4u > end - p) break;

        for (uint32_t i = 0; i < count; ++i, p += 4) {
            const uint32_t m = (head & 0x40000000u) ? method : (method + i * 4u);
            const uint32_t v = kernel::R32(p);
            if (m == kInlineArray) {
                if (prim) vertices.push_back(v);
            } else if (m != kBeginEnd) {
                std::lock_guard<std::recursive_mutex> lock(d3d::Lock());
                ApplySimpleMethodToState(d3d::State(), m, v);
            } else if (v != 0) {
                prim = v;
                vertices.clear();
            } else if (prim != 0) {
                // Several push callers restore the prior VertexShader right after EndPush;
                // temporarily bind the draw's captured VS/FVF so stride and inputs match.
                uint32_t post_vs_handle = 0;
                std::shared_ptr<gpu::VertexShader> post_vs;
                uint32_t stride = 0;
                {
                    std::lock_guard<std::recursive_mutex> lock(d3d::Lock());
                    auto& s = d3d::State();
                    post_vs_handle = s.vs_handle;
                    post_vs = s.vs;
                    if (saved_push_vs_handle != 0) {
                        s.vs_handle = saved_push_vs_handle;
                        s.vs = saved_push_vs;
                    }
                    stride = CurrentVertexStrideBytes(s);
                }
                ApplyRenderAccuracy();
                const uint32_t bytes = uint32_t(vertices.size()) * 4u;
                if (stride && bytes >= stride && (bytes % stride) == 0) {
                    const uint32_t vert_count = bytes / stride;
                    uint32_t guest_hdr = 0;
                    const uint32_t guest_verts =
                        AllocDiupContiguousVertices(ctx, base, vertices.data(), bytes, guest_hdr);
                    (void)guest_hdr;
                    if (guest_verts) {
                        const uint32_t fl = ++g_push_flushes;
                        if (fl <= 8) {
                            LOGR("render_accuracy: PushFlush[%s] prim=%u verts=%u stride=%u vs=%08X",
                                 tag, prim, vert_count, stride, saved_push_vs_handle);
                        }
                        const uint32_t saved_eax = ctx.eax;
                        const uint32_t saved_ecx = ctx.ecx;
                        const uint32_t saved_edx = ctx.edx;
                        X86_PUSH32(stride);
                        X86_PUSH32(guest_verts);
                        X86_PUSH32(vert_count);
                        X86_PUSH32(prim);
                        X86_PUSH32(0u);
                        sub_00417570(ctx, base);
                        ctx.eax = saved_eax;
                        ctx.ecx = saved_ecx;
                        ctx.edx = saved_edx;
                    }
                }
                {
                    std::lock_guard<std::recursive_mutex> lock(d3d::Lock());
                    auto& s = d3d::State();
                    s.vs_handle = post_vs_handle;
                    s.vs = std::move(post_vs);
                }
                prim = 0;
                vertices.clear();
            }
        }
    }
}

void ApplyRenderAccuracy() {
    InitRenderAccuracy();
    ++g_apply_calls;

    std::lock_guard<std::recursive_mutex> lock(d3d::Lock());
    gpu::State& s = d3d::State();

    PullDeferredFromGuest(s);

    std::array<gpu::TextureDesc, 4> tex_descs{};
    std::array<bool, 4> tex_ok{};
    SyncStageTexturesAndPalettes(s, tex_descs, tex_ok);

    if (s.ps != nullptr) {
        SyncProgrammablePixelShader(s, tex_descs, tex_ok);
    } else {
        SyncFixedFunctionStages(s, tex_ok);
    }

    LogUniqueDrawState(s, tex_descs, tex_ok);
}

void OnSwapDiagnostics() {
    const uint64_t f = ++g_swap_count;
    if (f == 1 || (f % 300u) == 0u) {
        LOGR("render_accuracy: swap=%llu apply=%llu ffFix=%llu psFix=%llu texFix=%llu palFix=%llu push=%llu",
             static_cast<unsigned long long>(f),
             static_cast<unsigned long long>(g_apply_calls.load()),
             static_cast<unsigned long long>(g_ff_repairs.load()),
             static_cast<unsigned long long>(g_ps_mode_repairs.load()),
             static_cast<unsigned long long>(g_tex_recoveries.load()),
             static_cast<unsigned long long>(g_pal_recoveries.load()),
             static_cast<unsigned long long>(g_push_flushes.load()));
    }
}

void EnsureKernelCompatHooks() {
    static std::once_flag k_once;
    std::call_once(k_once, [] {
        // Override ordinal 76 (IoQueryVolumeInformation) with proper XDK argument order:
        // NTSTATUS IoQueryVolumeInformation(PFILE_OBJECT FileObj, ULONG FsInfoClass, ULONG Len, PVOID Out, PULONG RetLen)
        kernel::RegisterExport(76, "IoQueryVolumeInformation", [](X86Context& ctx, uint8_t*) {
            const uint32_t cls = SafeR32(ctx.esp + 8u);
            const uint32_t len = SafeR32(ctx.esp + 12u);
            const uint32_t out = SafeR32(ctx.esp + 16u);
            const uint32_t ret_len = SafeR32(ctx.esp + 20u);
            uint32_t written = 0;
            uint32_t status = 0;

            if (out && len && CanAccessGuest(out, len)) {
                std::memset(kernel::HostPtr(out), 0, len);
                if (cls == 5 /* FileFsAttributeInformation */ && len >= 16) {
                    kernel::W32(out + 0, 0);
                    kernel::W32(out + 4, 42);
                    kernel::W32(out + 8, 4);
                    kernel::W32(out + 12, 0x58544146u);  // "FATX"
                    written = 16;
                } else if (cls == 1 /* FileFsVolumeInformation */ && len >= 16) {
                    kernel::W32(out + 8, 0x12345678u);
                    written = 16;
                } else if (cls == 3 /* FileFsSizeInformation */ && len >= 24) {
                    kernel::W32(out + 0, 0x00100000u);
                    kernel::W32(out + 8, 0x00080000u);
                    kernel::W32(out + 16, 32u);
                    kernel::W32(out + 20, 512u);
                    written = 24;
                } else if (cls == 4 /* FileFsDeviceInformation */ && len >= 8) {
                    kernel::W32(out + 0, 7u);
                    written = 8;
                } else {
                    status = 0xC000000Du;
                }
            } else {
                status = 0xC000000Du;
            }
            if (ret_len) SafeW32(ret_len, written);
            kernel::Return(ctx, status, 5);
        });
        LOGR("render_accuracy: installed fixed IoQueryVolumeInformation (ordinal 76) export");
    });
}

void InitRenderAccuracy() {
    std::call_once(g_init_once, [] {
        g_initialized.store(true, std::memory_order_release);
        EnsureKernelCompatHooks();
        LOGR("render_accuracy: initialized (MC3DR Xbox D3D8/NV2A HLE+Combiner Accuracy Engine)");
    });
}

}  // namespace render_accuracy
}  // namespace game
