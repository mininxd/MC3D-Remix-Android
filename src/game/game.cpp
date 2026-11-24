#include "app/app.h"
#include "common/options.h"
#include "core/log.h"
#include "cpu/x86_ops.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "sound/sound.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

    // Ensure all critical partition mappings and symbolic links exist for Midnight Club 3
    kernel::CreateSymbolicLink("\\??\\D:", "\\Device\\CdRom0");
    kernel::CreateSymbolicLink("\\??\\Z:", "\\Device\\Harddisk0\\Partition5");
    kernel::CreateSymbolicLink("\\??\\T:", "\\Device\\Harddisk0\\Partition1\\TDATA\\54540079");
    kernel::CreateSymbolicLink("\\??\\U:", "\\Device\\Harddisk0\\Partition1\\UDATA\\54540079");
    kernel::CreateSymbolicLink("\\Device\\Harddisk0\\Partition0", "\\Device\\Harddisk0\\Partition1");

    // Ensure network link status export (PhyGetLinkState, ordinal 252) returns active link
    kernel::RegisterExport(252, "PhyGetLinkState", [](X86Context& ctx, uint8_t*) {
        LOGI("kernel: PhyGetLinkState -> returning link up (0x0B)");
        // 0x0B = ETHERNET_LINK_ACTIVE (1) | ETHERNET_LINK_100MBPS (2) | ETHERNET_LINK_FULL_DUPLEX (8)
        kernel::Return(ctx, 0x0Bu, 1);
    });
    kernel::RegisterExport(253, "PhyInitialize", [](X86Context& ctx, uint8_t*) {
        kernel::Return(ctx, 0u, 2);
    });

    // Ordinal 76: IoQueryVolumeInformation (5 args, stdcall = 20 bytes stack)
    // NTSTATUS IoQueryVolumeInformation(HANDLE FileHandle, PIO_STATUS_BLOCK IoStatusBlock, PVOID FsInformation, ULONG Length, FS_INFORMATION_CLASS FsInformationClass);
    kernel::RegisterExport(76, "IoQueryVolumeInformation", [](X86Context& ctx, uint8_t*) {
        uint32_t handle = kernel::R32(ctx.esp + 4);
        uint32_t p_io_status = kernel::R32(ctx.esp + 8);
        uint32_t p_fs_info = kernel::R32(ctx.esp + 12);
        uint32_t length = kernel::R32(ctx.esp + 16);
        uint32_t fs_info_class = kernel::R32(ctx.esp + 20);

        LOGI("kernel: IoQueryVolumeInformation(handle=0x%X, class=%u, len=%u)", handle, fs_info_class, length);

        if (p_io_status) {
            kernel::W32(p_io_status, 0); // Status = STATUS_SUCCESS
            kernel::W32(p_io_status + 4, length); // Information = bytes written
        }

        if (p_fs_info && length >= 4) {
            if (fs_info_class == 5 /* FileFsAttributeInformation */) {
                // FILE_FS_ATTRIBUTE_INFORMATION:
                // ULONG FileSystemAttributes (0x00)
                // LONG MaximumComponentNameLength (0x04) = 42
                // ULONG FileSystemNameLength (0x08) = 4
                // CHAR FileSystemName[4] (0x0C) = "FATX" (0x58544146)
                kernel::W32(p_fs_info, 0);
                if (length >= 8) kernel::W32(p_fs_info + 4, 42);
                if (length >= 12) kernel::W32(p_fs_info + 8, 4);
                if (length >= 16) kernel::W32(p_fs_info + 12, 0x58544146u);
            } else if (fs_info_class == 1 /* FileFsVolumeInformation */) {
                if (length >= 12) kernel::W32(p_fs_info + 8, 0x12345678u);
                if (length >= 16) kernel::W32(p_fs_info + 12, 0);
            } else if (fs_info_class == 3 /* FileFsSizeInformation */) {
                if (length >= 24) {
                    kernel::W32(p_fs_info, 0x00100000u);
                    kernel::W32(p_fs_info + 4, 0);
                    kernel::W32(p_fs_info + 8, 0x00080000u);
                    kernel::W32(p_fs_info + 12, 0);
                    kernel::W32(p_fs_info + 16, 32); // 16KB clusters
                    kernel::W32(p_fs_info + 20, 512); // sector size
                }
            } else if (fs_info_class == 4 /* FileFsDeviceInformation */) {
                kernel::W32(p_fs_info, 7); // FILE_DEVICE_DISK
                if (length >= 8) kernel::W32(p_fs_info + 4, 0);
            }
        }

        kernel::Return(ctx, 0u, 5);
    });
}

}  // namespace game

// DirectSound stream/voice parameter setter null guard to prevent crash at 0x10000000b
X86_FUNC(sub_003C0DC5) {
    uint32_t arg1 = X86_MEM32(ctx.esp + 4);
    uint32_t arg2 = X86_MEM32(ctx.esp + 8);
    if (arg1 >= 0x10000u) {
        uint32_t obj = X86_MEM32(arg1 + 0x10);
        if (obj >= 0x10000u) {
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

// Voice unlinking null guard
X86_EXTERN_FUNC(__imp__sub_003C0400);
X86_FUNC(sub_003C0400) {
    uint32_t eax = X86_MEM32(ctx.esp + 4);
    if (eax < 0x10000u) { ctx.esp += 8; return; }
    uint32_t ecx = X86_MEM32(eax);
    uint32_t edx = X86_MEM32(eax + 4);
    if (ecx < 0x10000u || edx < 0x10000u) { ctx.esp += 8; return; }
    __imp__sub_003C0400(ctx, base);
}

// Voice mixbins setup null guard
X86_EXTERN_FUNC(__imp__sub_003C0AE2);
X86_FUNC(sub_003C0AE2) {
    uint32_t esi = X86_MEM32(ctx.esp + 4);
    if (esi < 0x10000u || ctx.ecx < 0x10000u) { ctx.esp += 8; return; }
    __imp__sub_003C0AE2(ctx, base);
}

// Voice destructor null guard
X86_EXTERN_FUNC(__imp__sub_003C0B75);
X86_FUNC(sub_003C0B75) {
    if (ctx.ecx < 0x10000u) { ctx.esp += 4; return; }
    __imp__sub_003C0B75(ctx, base);
}

// DirectSound voice / stream null guards to prevent crashes at 0x100000018 and siblings
X86_EXTERN_FUNC(sub_003C772E);
X86_FUNC(sub_003C0BC2) {
    uint32_t eax = X86_MEM32(ctx.esp + 4);
    uint32_t edx = X86_MEM32(ctx.esp + 8);
    if (eax >= 0x10000u) {
        uint32_t ecx = X86_MEM32(eax + 0x10);
        if (ecx >= 0x10000u) {
            X86_MEM32(ecx + 0x18) = edx;
        }
        uint32_t ctrl = X86_MEM32(eax + 0x0C);
        if (ctrl >= 0x10000u) {
            ctx.ecx = ctrl;
            X86_PUSH32(0x3C0BD8u);
            sub_003C772E(ctx, base);
        }
    }
    ctx.esp += 12;
}

X86_EXTERN_FUNC(sub_003C7696);
X86_FUNC(sub_003C0BDB) {
    uint32_t ecx = X86_MEM32(ctx.esp + 4);
    uint32_t edx = X86_MEM32(ctx.esp + 8);
    if (ecx >= 0x10000u) {
        uint32_t eax = X86_MEM32(ecx + 0x10);
        if (eax >= 0x10000u) {
            edx -= X86_MEM32(eax + 0x20);
            X86_MEM32(eax + 0x1C) = edx;
        }
        uint32_t ctrl = X86_MEM32(ecx + 0x0C);
        if (ctrl >= 0x10000u) {
            ctx.ecx = ctrl;
            X86_PUSH32(0x3C0BF4u);
            sub_003C7696(ctx, base);
        }
    }
    ctx.esp += 12;
}

X86_EXTERN_FUNC(__imp__sub_003C0BF7);
X86_FUNC(sub_003C0BF7) {
    uint32_t eax = X86_MEM32(ctx.esp + 4);
    if (eax < 0x10000u) { ctx.esp += 12; return; }
    __imp__sub_003C0BF7(ctx, base);
}

X86_EXTERN_FUNC(__imp__sub_003C0C0A);
X86_FUNC(sub_003C0C0A) {
    uint32_t eax = X86_MEM32(ctx.esp + 4);
    if (eax < 0x10000u) { ctx.esp += 12; return; }
    __imp__sub_003C0C0A(ctx, base);
}

X86_EXTERN_FUNC(__imp__sub_003C0C1D);
X86_FUNC(sub_003C0C1D) {
    uint32_t eax = X86_MEM32(ctx.esp + 4);
    if (eax < 0x10000u) { ctx.esp += 12; return; }
    __imp__sub_003C0C1D(ctx, base);
}

X86_FUNC(sub_003C0C30) {
    uint32_t edx = X86_MEM32(ctx.esp + 4);
    uint32_t esi = X86_MEM32(ctx.esp + 8);
    if (edx >= 0x10000u) {
        uint32_t eax = X86_MEM32(edx + 0x10);
        if (eax >= 0x10000u) {
            uint32_t ecx = X86_MEM32(eax + 0x20);
            ecx -= esi;
            X86_MEM32(eax + 0x1C) += ecx;
            X86_MEM32(eax + 0x20) = esi;
        }
        uint32_t ctrl = X86_MEM32(edx + 0x0C);
        if (ctrl >= 0x10000u) {
            ctx.ecx = ctrl;
            X86_PUSH32(0x3C0C4Fu);
            sub_003C7696(ctx, base);
        }
    }
    ctx.esp += 12;
}

X86_FUNC(sub_003C0C53) {
    uint32_t esi = X86_MEM32(ctx.esp + 4);
    uint32_t arg2 = X86_MEM32(ctx.esp + 8);
    if (esi >= 0x10000u) {
        uint32_t ecx = X86_MEM32(esi + 0x10);
        if (ecx >= 0x10000u && arg2 != 0) {
            ctx.ecx = ecx;
            X86_PUSH32(arg2);
            X86_PUSH32(0x3C0C64u);
            sub_003C0AE2(ctx, base);
        }
        uint32_t ctrl = X86_MEM32(esi + 0x0C);
        if (ctrl >= 0x10000u) {
            ctx.ecx = ctrl;
            X86_PUSH32(0x3C0C6Cu);
            sub_003C7696(ctx, base);
        }
    }
    ctx.esp += 12;
}

X86_EXTERN_FUNC(__imp__sub_003C0C70);
X86_FUNC(sub_003C0C70) {
    uint32_t eax = X86_MEM32(ctx.esp + 4);
    if (eax < 0x10000u) { ctx.eax = 0; ctx.esp += 8; return; }
    __imp__sub_003C0C70(ctx, base);
}

X86_EXTERN_FUNC(__imp__sub_003C0C81);
X86_FUNC(sub_003C0C81) {
    uint32_t eax = X86_MEM32(ctx.esp + 4);
    if (eax < 0x10000u) { ctx.esp += 12; return; }
    __imp__sub_003C0C81(ctx, base);
}

X86_EXTERN_FUNC(__imp__sub_003C0C94);
X86_FUNC(sub_003C0C94) {
    uint32_t ecx = X86_MEM32(ctx.esp + 4);
    if (ecx < 0x10000u) {
        ctx.eax = 0;
        ctx.esp += 12;
        return;
    }
    uint32_t eax = X86_MEM32(ecx + 0x10);
    if (eax < 0x10000u || X86_MEM32(eax + 0xB8) == 0) {
        ctx.eax = 0;
        ctx.esp += 12;
        return;
    }
    __imp__sub_003C0C94(ctx, base);
}

// Hardware voice routine null guards (0x100000071 / 0x100000012)
X86_EXTERN_FUNC(__imp__sub_003C772E);
X86_FUNC(sub_003C772E) {
    if (ctx.ecx < 0x10000u) {
        ctx.eax = 0;
        ctx.esp += 4;
        return;
    }
    uint32_t p = X86_MEM32(ctx.ecx + 0x70u);
    if (p < 0x10000u) {
        ctx.eax = 0;
        ctx.esp += 4;
        return;
    }
    __imp__sub_003C772E(ctx, base);
}

X86_EXTERN_FUNC(__imp__sub_003C7696);
X86_FUNC(sub_003C7696) {
    if (ctx.ecx < 0x10000u) {
        ctx.eax = 0;
        ctx.esp += 4;
        return;
    }
    __imp__sub_003C7696(ctx, base);
}

// DirectSound Buffer & Stream host implementations for methods that receive host-allocated Voice* objects

// IDirectSoundBuffer::SetPitch(this, lPitch)
X86_FUNC(sub_003C2090) {
    uint32_t obj = X86_MEM32(ctx.esp + 4);
    int32_t pitch = static_cast<int32_t>(X86_MEM32(ctx.esp + 8));
    std::lock_guard<std::recursive_mutex> lock(sound::g_lock);
    sound::Voice* v = sound::Find(obj);
    if (v) {
        v->freq = static_cast<uint32_t>(std::clamp<double>(
            double(v->rate) * std::pow(2.0, double(pitch) / 1200.0), 100.0, 192000.0));
    }
    ctx.eax = 0;
    ctx.esp += 12;
}

// IDirectSoundBuffer::SetPan(this, lPan)
X86_FUNC(sub_003C20AC) {
    ctx.eax = 0;
    ctx.esp += 12;
}

// IDirectSoundBuffer::SetEG(this, pEG)
X86_FUNC(sub_003C20C8) {
    ctx.eax = 0;
    ctx.esp += 12;
}

// IDirectSoundBuffer::SetFilter(this, pFilter)
X86_FUNC(sub_003C2240) {
    ctx.eax = 0;
    ctx.esp += 12;
}

X86_FUNC(sub_003C225C) {
    ctx.eax = 0;
    ctx.esp += 12;
}

X86_FUNC(sub_003C2278) {
    ctx.eax = 0;
    ctx.esp += 12;
}

// CDirectSoundStream::SetPitch(this, lPitch)
X86_FUNC(sub_003C2299) {
    uint32_t obj = X86_MEM32(ctx.esp + 4);
    int32_t pitch = static_cast<int32_t>(X86_MEM32(ctx.esp + 8));
    std::lock_guard<std::recursive_mutex> lock(sound::g_lock);
    sound::Voice* v = sound::Find(obj);
    if (v) {
        v->freq = static_cast<uint32_t>(std::clamp<double>(
            double(v->rate) * std::pow(2.0, double(pitch) / 1200.0), 100.0, 192000.0));
    }
    ctx.eax = 0;
    ctx.esp += 12;
}

// CDirectSoundStream::SetPan(this, lPan)
X86_FUNC(sub_003C229E) {
    ctx.eax = 0;
    ctx.esp += 12;
}

// CDirectSoundStream::SetEG(this, pEG)
X86_FUNC(sub_003C22A3) {
    ctx.eax = 0;
    ctx.esp += 12;
}

// CDirectSoundStream::SetFilter(this, pFilter)
X86_FUNC(sub_003C22D4) {
    ctx.eax = 0;
    ctx.esp += 12;
}

X86_FUNC(sub_003C22D9) {
    ctx.eax = 0;
    ctx.esp += 12;
}

X86_FUNC(sub_003C22DE) {
    ctx.eax = 0;
    ctx.esp += 12;
}

// CDirectSoundStream::SetVolume(this, lVolume)
X86_FUNC(sub_003C1FAB) {
    uint32_t obj = X86_MEM32(ctx.esp + 4);
    int32_t vol = static_cast<int32_t>(X86_MEM32(ctx.esp + 8));
    std::lock_guard<std::recursive_mutex> lock(sound::g_lock);
    sound::Voice* v = sound::Find(obj);
    if (v) {
        v->volume = sound::Db(vol);
    }
    ctx.eax = 0;
    ctx.esp += 12;
}

X86_FUNC(sub_003C2933) {
    ctx.eax = 0;
    ctx.esp += 12;
}

X86_FUNC(sub_003C36E9) {
    ctx.eax = 0;
    ctx.esp += 20;
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
#if HAS_D3D_FvfShader
        X86_MEM32(esi + 0x794) = SYM(D3D_FvfShader);
        eax = SYM(D3D_FvfShader);
#else
        X86_MEM32(esi + 0x794) = 0x00425400u;
        eax = 0x00425400u;
#endif
    }
    if (!mem::IsCommitted(eax + 4)) {
        ctx.esp += 4;
        return;
    }
    __imp__sub_00415440(ctx, base);
}

// Sound engine voice & stream unallocated channel safety guards
X86_EXTERN_FUNC(__imp__sub_0040F198);
X86_FUNC(sub_0040F198) {
    uint32_t esi = ctx.ecx;
    if (esi < 0x10000u) {
        ctx.eax = 0;
        ctx.esp += 4;
        return;
    }
    uint32_t buf = X86_MEM32(esi + 0x1Cu);
    uint32_t strm = X86_MEM32(esi + 0x20u);
    if (!buf && !strm) {
        ctx.eax = 0;
        ctx.esp += 4;
        return;
    }
    if (!buf && strm) {
        if (strm < 0x10000u || !mem::IsCommitted(strm)) {
            ctx.eax = 0;
            ctx.esp += 4;
            return;
        }
        uint32_t vt = X86_MEM32(strm);
        if (vt < 0x10000u || !mem::IsCommitted(vt + 0x0Cu)) {
            ctx.eax = 0;
            ctx.esp += 4;
            return;
        }
    }
    __imp__sub_0040F198(ctx, base);
}

X86_EXTERN_FUNC(__imp__sub_0040F6EF);
X86_FUNC(sub_0040F6EF) {
    uint32_t esi = ctx.ecx;
    if (esi < 0x10000u) {
        ctx.eax = 0;
        ctx.esp += 8;
        return;
    }
    uint32_t buf = X86_MEM32(esi + 0x1Cu);
    uint32_t strm = X86_MEM32(esi + 0x20u);
    if (!buf && !strm) {
        uint32_t out_ptr = X86_MEM32(ctx.esp + 4);
        if (out_ptr >= 0x10000u && mem::IsCommitted(out_ptr)) {
            X86_MEM32(out_ptr) = 0;
        }
        ctx.eax = 0;
        ctx.esp += 8;
        return;
    }
    if (!buf && strm) {
        if (strm < 0x10000u || !mem::IsCommitted(strm)) {
            uint32_t out_ptr = X86_MEM32(ctx.esp + 4);
            if (out_ptr >= 0x10000u && mem::IsCommitted(out_ptr)) {
                X86_MEM32(out_ptr) = 0;
            }
            ctx.eax = 0;
            ctx.esp += 8;
            return;
        }
        uint32_t vt = X86_MEM32(strm);
        if (vt < 0x10000u || !mem::IsCommitted(vt + 0x0Cu)) {
            uint32_t out_ptr = X86_MEM32(ctx.esp + 4);
            if (out_ptr >= 0x10000u && mem::IsCommitted(out_ptr)) {
                X86_MEM32(out_ptr) = 0;
            }
            ctx.eax = 0;
            ctx.esp += 8;
            return;
        }
    }
    __imp__sub_0040F6EF(ctx, base);
}

X86_EXTERN_FUNC(__imp__sub_0040F735);
X86_FUNC(sub_0040F735) {
    uint32_t esi = ctx.ecx;
    if (esi < 0x10000u) { ctx.eax = 0; ctx.esp += 12; return; }
    uint32_t strm = X86_MEM32(esi + 0x20u);
    if (strm < 0x10000u || !mem::IsCommitted(strm)) { ctx.eax = 0; ctx.esp += 12; return; }
    uint32_t vt = X86_MEM32(strm);
    if (vt < 0x10000u || !mem::IsCommitted(vt + 0x10u)) { ctx.eax = 0; ctx.esp += 12; return; }
    __imp__sub_0040F735(ctx, base);
}

X86_EXTERN_FUNC(__imp__sub_0040F76F);
X86_FUNC(sub_0040F76F) {
    uint32_t esi = ctx.ecx;
    if (esi < 0x10000u) { ctx.eax = 0; ctx.esp += 4; return; }
    uint32_t strm = X86_MEM32(esi + 0x20u);
    if (strm < 0x10000u || !mem::IsCommitted(strm)) { ctx.eax = 0; ctx.esp += 4; return; }
    uint32_t vt = X86_MEM32(strm);
    if (vt < 0x10000u || !mem::IsCommitted(vt + 0x18u)) { ctx.eax = 0; ctx.esp += 4; return; }
    __imp__sub_0040F76F(ctx, base);
}

X86_EXTERN_FUNC(__imp__sub_0040F7DD);
X86_FUNC(sub_0040F7DD) {
    uint32_t esi = ctx.ecx;
    if (esi < 0x10000u) { ctx.eax = 0; ctx.esp += 4; return; }
    uint32_t strm = X86_MEM32(esi + 0x20u);
    if (strm < 0x10000u || !mem::IsCommitted(strm)) { ctx.eax = 0; ctx.esp += 4; return; }
    uint32_t vt = X86_MEM32(strm);
    if (vt < 0x10000u || !mem::IsCommitted(vt + 0x14u)) { ctx.eax = 0; ctx.esp += 4; return; }
    __imp__sub_0040F7DD(ctx, base);
}

X86_EXTERN_FUNC(__imp__sub_0040F7A1);
X86_FUNC(sub_0040F7A1) {
    uint32_t esi = ctx.ecx;
    if (esi < 0x10000u) { ctx.eax = 0; ctx.esp += 16; return; }
    uint32_t strm = X86_MEM32(esi + 0x20u);
    if (strm < 0x10000u || !mem::IsCommitted(strm)) { ctx.eax = 0; ctx.esp += 16; return; }
    __imp__sub_0040F7A1(ctx, base);
}

X86_EXTERN_FUNC(__imp__sub_0040F229);
X86_FUNC(sub_0040F229) {
    uint32_t esi = ctx.ecx;
    if (esi < 0x10000u) { ctx.eax = 0; ctx.esp += 8; return; }
    uint32_t buf = X86_MEM32(esi + 0x1Cu);
    uint32_t strm = X86_MEM32(esi + 0x20u);
    if (!buf && (!strm || strm < 0x10000u || !mem::IsCommitted(strm))) {
        ctx.eax = 0;
        ctx.esp += 8;
        return;
    }
    __imp__sub_0040F229(ctx, base);
}

X86_EXTERN_FUNC(__imp__sub_0040F304);
X86_FUNC(sub_0040F304) {
    uint32_t esi = ctx.ecx;
    if (esi < 0x10000u) { ctx.eax = 0; ctx.esp += 8; return; }
    uint32_t buf = X86_MEM32(esi + 0x1Cu);
    uint32_t strm = X86_MEM32(esi + 0x20u);
    if (!buf && (!strm || strm < 0x10000u || !mem::IsCommitted(strm))) {
        ctx.eax = 0;
        ctx.esp += 8;
        return;
    }
    __imp__sub_0040F304(ctx, base);
}

X86_EXTERN_FUNC(__imp__sub_0040F34A);
X86_FUNC(sub_0040F34A) {
    uint32_t esi = ctx.ecx;
    if (esi < 0x10000u) { ctx.eax = 0; ctx.esp += 12; return; }
    uint32_t buf = X86_MEM32(esi + 0x1Cu);
    uint32_t strm = X86_MEM32(esi + 0x20u);
    if (!buf && (!strm || strm < 0x10000u || !mem::IsCommitted(strm))) {
        ctx.eax = 0;
        ctx.esp += 12;
        return;
    }
    __imp__sub_0040F34A(ctx, base);
}

X86_EXTERN_FUNC(__imp__sub_0040F390);
X86_FUNC(sub_0040F390) {
    uint32_t esi = ctx.ecx;
    if (esi < 0x10000u) { ctx.eax = 0; ctx.esp += 4; return; }
    uint32_t buf = X86_MEM32(esi + 0x1Cu);
    uint32_t strm = X86_MEM32(esi + 0x20u);
    if (!buf && (!strm || strm < 0x10000u || !mem::IsCommitted(strm))) {
        ctx.eax = 0;
        ctx.esp += 4;
        return;
    }
    __imp__sub_0040F390(ctx, base);
}

X86_EXTERN_FUNC(__imp__sub_0040F3DA);
X86_FUNC(sub_0040F3DA) {
    uint32_t esi = ctx.ecx;
    if (esi < 0x10000u) { ctx.eax = 0; ctx.esp += 12; return; }
    uint32_t buf = X86_MEM32(esi + 0x1Cu);
    uint32_t strm = X86_MEM32(esi + 0x20u);
    if (!buf && (!strm || strm < 0x10000u || !mem::IsCommitted(strm))) {
        ctx.eax = 0;
        ctx.esp += 12;
        return;
    }
    __imp__sub_0040F3DA(ctx, base);
}

X86_EXTERN_FUNC(__imp__sub_0040F420);
X86_FUNC(sub_0040F420) {
    uint32_t esi = ctx.ecx;
    if (esi < 0x10000u) { ctx.eax = 0; ctx.esp += 12; return; }
    uint32_t buf = X86_MEM32(esi + 0x1Cu);
    uint32_t strm = X86_MEM32(esi + 0x20u);
    if (!buf && (!strm || strm < 0x10000u || !mem::IsCommitted(strm))) {
        ctx.eax = 0;
        ctx.esp += 12;
        return;
    }
    __imp__sub_0040F420(ctx, base);
}

X86_EXTERN_FUNC(__imp__sub_0040F466);
X86_FUNC(sub_0040F466) {
    uint32_t esi = ctx.ecx;
    if (esi < 0x10000u) { ctx.eax = 0; ctx.esp += 12; return; }
    uint32_t buf = X86_MEM32(esi + 0x1Cu);
    if (!buf || buf < 0x10000u || !mem::IsCommitted(buf)) {
        ctx.eax = 0;
        ctx.esp += 12;
        return;
    }
    __imp__sub_0040F466(ctx, base);
}

X86_EXTERN_FUNC(__imp__sub_0040F4DA);
X86_FUNC(sub_0040F4DA) {
    uint32_t esi = ctx.ecx;
    if (esi < 0x10000u) { ctx.eax = 0; ctx.esp += 12; return; }
    uint32_t buf = X86_MEM32(esi + 0x1Cu);
    uint32_t strm = X86_MEM32(esi + 0x20u);
    if (!buf && (!strm || strm < 0x10000u || !mem::IsCommitted(strm))) {
        ctx.eax = 0;
        ctx.esp += 12;
        return;
    }
    __imp__sub_0040F4DA(ctx, base);
}

X86_EXTERN_FUNC(__imp__sub_0040F520);
X86_FUNC(sub_0040F520) {
    uint32_t esi = ctx.ecx;
    if (esi < 0x10000u) { ctx.eax = 0; ctx.esp += 8; return; }
    uint32_t buf = X86_MEM32(esi + 0x1Cu);
    uint32_t strm = X86_MEM32(esi + 0x20u);
    if (!buf && (!strm || strm < 0x10000u || !mem::IsCommitted(strm))) {
        ctx.eax = 0;
        ctx.esp += 8;
        return;
    }
    __imp__sub_0040F520(ctx, base);
}

X86_EXTERN_FUNC(__imp__sub_0040F566);
X86_FUNC(sub_0040F566) {
    uint32_t esi = ctx.ecx;
    if (esi < 0x10000u) { ctx.eax = 0; ctx.esp += 8; return; }
    uint32_t buf = X86_MEM32(esi + 0x1Cu);
    uint32_t strm = X86_MEM32(esi + 0x20u);
    if (!buf && (!strm || strm < 0x10000u || !mem::IsCommitted(strm))) {
        ctx.eax = 0;
        ctx.esp += 8;
        return;
    }
    __imp__sub_0040F566(ctx, base);
}

X86_EXTERN_FUNC(__imp__sub_0040F5AC);
X86_FUNC(sub_0040F5AC) {
    uint32_t esi = ctx.ecx;
    if (esi < 0x10000u) { ctx.eax = 0; ctx.esp += 8; return; }
    uint32_t buf = X86_MEM32(esi + 0x1Cu);
    uint32_t strm = X86_MEM32(esi + 0x20u);
    if (!buf && (!strm || strm < 0x10000u || !mem::IsCommitted(strm))) {
        ctx.eax = 0;
        ctx.esp += 8;
        return;
    }
    __imp__sub_0040F5AC(ctx, base);
}

X86_EXTERN_FUNC(__imp__sub_0040F663);
X86_FUNC(sub_0040F663) {
    uint32_t esi = ctx.ecx;
    if (esi < 0x10000u) { ctx.eax = 0; ctx.esp += 8; return; }
    uint32_t buf = X86_MEM32(esi + 0x1Cu);
    uint32_t strm = X86_MEM32(esi + 0x20u);
    if (!buf && (!strm || strm < 0x10000u || !mem::IsCommitted(strm))) {
        ctx.eax = 0;
        ctx.esp += 8;
        return;
    }
    __imp__sub_0040F663(ctx, base);
}

X86_EXTERN_FUNC(__imp__sub_0040F6A9);
X86_FUNC(sub_0040F6A9) {
    uint32_t esi = ctx.ecx;
    if (esi < 0x10000u) { ctx.eax = 0; ctx.esp += 8; return; }
    uint32_t buf = X86_MEM32(esi + 0x1Cu);
    uint32_t strm = X86_MEM32(esi + 0x20u);
    if (!buf && (!strm || strm < 0x10000u || !mem::IsCommitted(strm))) {
        ctx.eax = 0;
        ctx.esp += 8;
        return;
    }
    __imp__sub_0040F6A9(ctx, base);
}

// Native CRT memcpy / memmove replacement (0x002DB4C0)
// Overrides the weak recompiled CRT memcpy assembly routine.
// In MSVC CRT x86, memcpy is an alias for memmove (handles overlapping ranges).
// cdecl calling convention:
//   [esp + 4]  = dest
//   [esp + 8]  = src
//   [esp + 12] = count
// Return value: eax = dest.
// Callee pops return address (esp += 4), caller cleans arguments (add esp, 12).
X86_FUNC(sub_002DB4C0) {
    uint32_t dest  = X86_MEM32(ctx.esp + 4);
    uint32_t src   = X86_MEM32(ctx.esp + 8);
    uint32_t count = X86_MEM32(ctx.esp + 12);
    if (count != 0 && dest >= 0x10000u && src >= 0x10000u) {
        std::memmove(base + dest, base + src, count);
    }
    ctx.eax = dest;
    ctx.esp += 4;
}

// Fallback stubs for internal memcpy labels if ever targeted indirectly
X86_FUNC(sub_002DB61C) { ctx.esp += 4; }
X86_FUNC(sub_002DB624) { ctx.esp += 4; }
X86_FUNC(sub_002DB630) { ctx.esp += 4; }
X86_FUNC(sub_002DB644) { ctx.esp += 4; }

// ============================================================================
// Midnight Club 3: XDK CRT & Filesystem Initialization Bridges and Overrides
// ============================================================================

// sub_000142B9: Queries available free blocks for game saves on hard disk.
// In the original XDK binary, this queries XQueryVolumeInformation and returns the free block count in eax.
// Original calling convention: cdecl, 0 arguments (esp += 4 on return).
X86_FUNC(sub_000142B9) {
    LOGI("guest: sub_000142B9() -> reporting 0x80000 free blocks (8 GiB)");
    ctx.eax = 0x00080000u; // 524,288 blocks of 16KB = 8 GiB available free space
    ctx.esp += 4;
}

// Fallback label for sub_000142BD if ever targeted indirectly
X86_FUNC(sub_000142BD) {
    LOGI("guest: sub_000142BD() -> reporting 0x80000 free blocks (8 GiB)");
    ctx.eax = 0x00080000u;
    ctx.esp += 4;
}

// sub_0001461D (XMountUtilityDrive):
// Mounts utility partition 5 (\??\Z:) and returns 1 on success.
// Calling convention: stdcall with 1 argument (4 bytes) + return address (4 bytes) = 8 bytes popped.
X86_FUNC(sub_0001461D) {
    LOGI("guest: sub_0001461D (XMountUtilityDrive) -> mounting Z: and returning 1");
    std::string z_host = kernel::ResolvePath("\\Device\\Harddisk0\\Partition5", 0);
    std::error_code ec;
    if (!z_host.empty()) std::filesystem::create_directories(z_host, ec);
    kernel::CreateSymbolicLink("\\??\\Z:", "\\Device\\Harddisk0\\Partition5");
    ctx.eax = 1;
    ctx.esp += 8;
}

// sub_000130DE (CRT initialization):
// In the original binary, 0x13114 falls through into 0x13116 (sub_00013116).
// The recompiler split them into two functions, causing sub_000130DE to end abruptly without
// executing sub_00013116 or popping the callee-saved registers and returning.
X86_EXTERN_FUNC(__imp__sub_000130DE);
X86_EXTERN_FUNC(sub_00013116);
X86_FUNC(sub_000130DE) {
    LOGI("guest: sub_000130DE (CRT init) -> executing loop 1");
    __imp__sub_000130DE(ctx, base);
    LOGI("guest: sub_000130DE (CRT init) -> executing loop 2 (sub_00013116)");
    sub_00013116(ctx, base);
}

// sub_00012EA4: Mounts \??\T: (TDATA) and \??\U: (UDATA) for title 0x54540079.
// Calling convention: stdcall with 2 arguments (8 bytes) + return address (4 bytes) = 12 bytes.
X86_FUNC(sub_00012EA4) {
    uint32_t title_id = X86_MEM32(ctx.esp + 4);
    uint32_t name_ptr = X86_MEM32(ctx.esp + 8);
    LOGI("guest: sub_00012EA4(title_id=0x%08X, name_ptr=0x%08X) -> mounting T: and U:", title_id, name_ptr);
    std::string t_host = kernel::ResolvePath("\\Device\\Harddisk0\\Partition1\\TDATA\\54540079", 0);
    std::string u_host = kernel::ResolvePath("\\Device\\Harddisk0\\Partition1\\UDATA\\54540079", 0);
    std::error_code ec;
    if (!t_host.empty()) std::filesystem::create_directories(t_host, ec);
    if (!u_host.empty()) std::filesystem::create_directories(u_host, ec);
    kernel::CreateSymbolicLink("\\??\\T:", "\\Device\\Harddisk0\\Partition1\\TDATA\\54540079");
    kernel::CreateSymbolicLink("\\??\\U:", "\\Device\\Harddisk0\\Partition1\\UDATA\\54540079");
    ctx.eax = 0; // STATUS_SUCCESS
    ctx.esp += 12;
}

// sub_00012F5E: Verifies partition 1 cluster size is 16KB.
// Calling convention: stdcall with 1 argument (4 bytes) + return address (4 bytes) = 8 bytes.
X86_FUNC(sub_00012F5E) {
    LOGI("guest: sub_00012F5E() -> verifying cluster size (STATUS_SUCCESS)");
    ctx.eax = 0; // STATUS_SUCCESS
    ctx.esp += 8;
}

// sub_00012EF9: Fatal exit handler that reboots to the Xbox dashboard.
X86_EXTERN_FUNC(__imp__sub_00012EF9);
X86_FUNC(sub_00012EF9) {
    uint32_t arg1 = X86_MEM32(ctx.esp + 4);
    uint32_t arg2 = X86_MEM32(ctx.esp + 8);
    uint32_t arg3 = X86_MEM32(ctx.esp + 12);
    LOGE("guest: sub_00012EF9 (FATAL EXIT) called! arg1=%u, arg2=%u, arg3=0x%X", arg1, arg2, arg3);
    __imp__sub_00012EF9(ctx, base);
}

// sub_00012F6F: Hardware and filesystem pre-initialization wrapper.
X86_EXTERN_FUNC(__imp__sub_00012F6F);
X86_FUNC(sub_00012F6F) {
    LOGI("guest: sub_00012F6F (Hardware/FS Pre-Init) starting...");
    __imp__sub_00012F6F(ctx, base);
    LOGI("guest: sub_00012F6F (Hardware/FS Pre-Init) completed successfully");
}

// sub_00012150: Midnight Club 3 main() entry point wrapper.
X86_EXTERN_FUNC(__imp__sub_00012150);
X86_FUNC(sub_00012150) {
    LOGI("================================================================================");
    LOGI("guest: ENTERING MIDNIGHT CLUB 3: DUB EDITION REMIX MAIN LOOP (sub_00012150)");
    LOGI("================================================================================");
    __imp__sub_00012150(ctx, base);
    LOGI("guest: sub_00012150 returned");
}



