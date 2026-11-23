#include "app/app.h"
#include "common/options.h"
#include "core/log.h"
#include "cpu/x86_ops.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace game {

namespace {

std::atomic<bool> g_ticker_running{false};
std::thread g_ticker_thread;

void StartHardwareTicker() {
    if (g_ticker_running.exchange(true)) return;

    g_ticker_thread = std::thread([] {
        using clock = std::chrono::steady_clock;
        auto t0 = clock::now();
        uint32_t last_written = 0;

        while (g_ticker_running.load(std::memory_order_relaxed)) {
            uint8_t* base = mem::Base();
            if (base) {
                volatile uint32_t* p_xgscnt = reinterpret_cast<volatile uint32_t*>(base + 0xFE80200Cu);
                volatile uint32_t* p_gp_cnt = reinterpret_cast<volatile uint32_t*>(base + 0xFE820010u);

                auto now = clock::now();

                // If guest code reset the sequencer sample counter (e.g. wrote 0 to 0xFE80200C)
                uint32_t cur = *p_xgscnt;
                if (cur < last_written && (last_written - cur) > 1000) {
                    t0 = now - std::chrono::microseconds((uint64_t(cur) * 1000000ull) / 48000ull);
                }

                uint64_t elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(now - t0).count();
                uint32_t samples = static_cast<uint32_t>((elapsed_us * 48000ull) / 1000000ull);

                *p_xgscnt = samples;
                last_written = samples;

                // DirectSound SetupVoiceProcessor GP sample counter must advance past 4
                *p_gp_cnt = samples + 0x10u;

                // AC'97 Codec ready bit (bit 8 = 0x100 at 0xFEC00130)
                *reinterpret_cast<volatile uint32_t*>(base + 0xFEC00130u) |= 0x00000100u;

                // NV2A PFIFO empty flags
                *reinterpret_cast<volatile uint32_t*>(base + 0xFD002400u) |= 0x00000010u;
                *reinterpret_cast<volatile uint32_t*>(base + 0xFD003214u) |= 0x00000010u;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
    });
    g_ticker_thread.detach();
}

}  // namespace

void Loaded() {
    static std::atomic<bool> s_loaded{false};
    if (s_loaded.exchange(true)) return;

    LOGI("hardware: committing MMIO apertures for NV2A, MCPX APU, and Flash ROM...");

    // 1. NV2A GPU MMIO aperture: 0xFD000000 - 0xFE000000 (16 MB)
    if (!mem::Commit(0xFD000000u, 0x01000000u)) {
        LOGE("hardware: failed to commit NV2A aperture at 0xFD000000");
    }

    // 2. MCPX Southbridge aperture: 0xFE800000 - 0xFF000000 (8 MB)
    // Covers APU (0xFE800000), AC97 (0xFEC00000), USB (0xFED00000), NIC (0xFEF00000)
    if (!mem::Commit(0xFE800000u, 0x00800000u)) {
        LOGE("hardware: failed to commit MCPX aperture at 0xFE800000");
    }

    // 3. Flash ROM mirror: 0xFF000000 - 0xFFFFFFFF (16 MB)
    if (!mem::Commit(0xFF000000u, 0x01000000u)) {
        LOGE("hardware: failed to commit Flash ROM aperture at 0xFF000000");
    }

    uint8_t* base = mem::Base();
    if (base) {
        // Initial AC'97 codec ready bit
        *reinterpret_cast<volatile uint32_t*>(base + 0xFEC00130u) = 0x00000100u;

        // Initial NV2A idle status bits
        *reinterpret_cast<volatile uint32_t*>(base + 0xFD002400u) = 0x00000010u;
        *reinterpret_cast<volatile uint32_t*>(base + 0xFD003214u) = 0x00000010u;

        // Initial APU GP sample counter (must be >= 4 for SetupVoiceProcessor)
        *reinterpret_cast<volatile uint32_t*>(base + 0xFE820010u) = 0x00000010u;

        // Initial APU sequencer sample counter
        *reinterpret_cast<volatile uint32_t*>(base + 0xFE80200Cu) = 0x00000000u;
    }

    StartHardwareTicker();
    LOGI("hardware: MMIO apertures committed and APU/NV2A ticker running");

    // Ensure network link status export (PhyGetLinkState, ordinal 252) returns active link
    kernel::RegisterExport(252, "PhyGetLinkState", [](X86Context& ctx, uint8_t*) {
        LOGI("kernel: PhyGetLinkState -> returning link up (0x0B)");
        // 0x0B = ETHERNET_LINK_ACTIVE (1) | ETHERNET_LINK_100MBPS (2) | ETHERNET_LINK_FULL_DUPLEX (8)
        kernel::Return(ctx, 0x0Bu, 1);
    });
    kernel::RegisterExport(253, "PhyInitialize", [](X86Context& ctx, uint8_t*) {
        kernel::Return(ctx, 0u, 2);
    });
}

}  // namespace game

// DirectSound stream/voice parameter setter null guard to prevent crash at 0x10000000b
X86_FUNC(sub_003C0DC5) {
    uint32_t arg1 = X86_MEM32(ctx.esp + 4);
    uint32_t arg2 = X86_MEM32(ctx.esp + 8);
    if (arg1 != 0) {
        uint32_t obj = X86_MEM32(arg1 + 0x10);
        if (obj != 0) {
            if (arg2 != 0) {
                X86_MEM8(obj + 0x0B) |= 0x01;
            } else {
                X86_MEM8(obj + 0x0B) &= 0xFE;
            }
        }
    }
    ctx.eax = 0;
    ctx.zf = 1;
    ctx.cf = 0;
    ctx.sf = 0;
    ctx.of = 0;
    ctx.pf = 1;
    ctx.esp += 12;
}

// Fatal error interceptor: logs original guest error message to logcat & crash log
X86_FUNC(sub_00019070) {
    uint32_t fmt_ptr = X86_MEM32(ctx.esp + 4);
    const char* fmt = (fmt_ptr && base) ? reinterpret_cast<const char*>(base + fmt_ptr) : nullptr;
    if (fmt) {
        char buf[512];
        uint32_t a1 = X86_MEM32(ctx.esp + 8);
        uint32_t a2 = X86_MEM32(ctx.esp + 12);
        uint32_t a3 = X86_MEM32(ctx.esp + 16);
        std::snprintf(buf, sizeof(buf), fmt, a1, a2, a3);
        LOGE("FATAL ERROR from guest: %s", buf);
    } else {
        LOGE("FATAL ERROR from guest (no format string, fmt_ptr=0x%08X)", fmt_ptr);
    }
    std::abort();
}

// Direct3D 8 vertex shader null-dereference guard (0x100000004)
X86_EXTERN_FUNC(__imp__sub_00415440);
X86_FUNC(sub_00415440) {
    uint32_t esi = ctx.ecx;
    if (!esi || !mem::IsCommitted(esi + 0x794)) {
        ctx.esp += 4;
        return;
    }
    uint32_t eax = X86_MEM32(esi + 0x794);
    if (!eax) {
        X86_MEM32(esi + 0x794) = 0x00425400u;
        eax = 0x00425400u;
    }
    if (!mem::IsCommitted(eax + 4)) {
        ctx.esp += 4;
        return;
    }
    __imp__sub_00415440(ctx, base);
}

