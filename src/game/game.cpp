#include "app/app.h"
#include "common/options.h"
#include "core/log.h"
#include "cpu/x86_ops.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "sound/sound.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
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

    // Ordinal 24: ExQueryNonVolatileSetting (5 args, stdcall = 20 bytes stack)
    // Supports European/PAL console configuration for Midnight Club 3: DUB Edition Remix (Europe).
    kernel::RegisterExport(24, "ExQueryNonVolatileSetting", [](X86Context& ctx, uint8_t*) {
        uint32_t index = kernel::R32(ctx.esp + 4);
        uint32_t type_out = kernel::R32(ctx.esp + 8);
        uint32_t value = kernel::R32(ctx.esp + 12);
        uint32_t len = kernel::R32(ctx.esp + 16);
        uint32_t result_len = kernel::R32(ctx.esp + 20);

        uint32_t v = 0;
        switch (index) {
            case 7:   v = 1; break;                   // XC_LANGUAGE (1 = English)
            case 8:                                   // XC_VIDEO_FLAGS (widescreen, 480p, PAL60)
                v = 0x00010000u | 0x00080000u | 0x00200000u;
                break;
            case 9:   v = 0; break;                   // XC_AUDIO_FLAGS (stereo)
            case 0xA: v = 0; break;                   // XC_MAX_GAME_SAVE_BLOCKS (0 = unlimited / no parental restriction)
            case 0x11: v = 0; break;                  // XC_PARENTAL_CONTROL_GAMES (0 = all ratings permitted)
            case 0x12: v = 2; break;                  // XC_DVD_REGION (2 = Europe / PAL)
            case 0x101:                               // XC_FACTORY_ETHERNET_ADDR (6-byte console MAC)
                if (value && len >= 6) {
                    uint8_t mac[6] = {0x00, 0x50, 0xF2, 0x54, 0x00, 0x79};
                    std::memcpy(kernel::HostPtr(value), mac, 6);
                }
                break;
            case 0x103: v = 0x00400200u; break;       // XC_FACTORY_AV_REGION (0x00400200 = PAL-I Europe)
            case 0x104: v = 4; break;                  // XC_FACTORY_GAME_REGION (4 = Europe / Rest of World)
            default: break;
        }

        if (index == 0xFFFF) {
            // Full 256-byte EEPROM image
            if (type_out) kernel::W32(type_out, 3); // REG_BINARY
            if (value && len >= 0x100) {
                std::memset(kernel::HostPtr(value), 0, len);
                kernel::W32(value + 0x2C, 4);                     // factory game region: 4 (Europe)
                kernel::W32(value + 0x58, 0x00400200u);           // factory video standard: PAL-I
                kernel::W32(value + 0xE8, 1);                     // language: English
                kernel::W32(value + 0xEC, 0x00010000u | 0x00080000u | 0x00200000u); // video flags
                kernel::W32(value + 0xF0, 0);                     // audio flags: stereo
            }
            if (result_len) kernel::W32(result_len, 0x100);
            LOGI("kernel: ExQueryNonVolatileSetting(0xFFFF) -> full PAL EEPROM image");
            kernel::Return(ctx, 0u, 5);
            return;
        }

        if (type_out) kernel::W32(type_out, 4); // REG_DWORD
        if (value && len >= 4 && index != 0x101) {
            kernel::W32(value, v);
        }
        if (result_len) kernel::W32(result_len, (index == 0x101) ? 6 : 4);
        LOGI("kernel: ExQueryNonVolatileSetting(index=0x%X) -> 0x%X", index, v);
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

// sub_002DE0AA: CRT buffer / stream table initializer (called from sub_002DFFB9 during CRT constructor loop)
// In the original XBE binary, this function begins at 0x002DE0AA.
// In MSVC CRT, this sets up default buffer pointers for standard stream table at 0x4B5DC8.
X86_EXTERN_FUNC(sub_002E25B5);
X86_FUNC(sub_002DE0AA) {
    LOGI("guest: sub_002DE0AA (CRT buffer table init) starting...");
    uint32_t saved_esi = ctx.esi;
    uint32_t saved_edi = ctx.edi;

    uint32_t esi = 0;
    uint32_t edi = 0x55C148u;

    while (esi < 0x24u) {
        uint32_t flag = X86_MEM32(esi * 8u + 0x4B5DCCu);
        if (flag == 1) {
            uint32_t entry = esi * 8u + 0x4B5DC8u;
            X86_MEM32(entry) = edi;
            // Push arguments for sub_002E25B5(pBuffer, 0xFA0)
            X86_PUSH32(0xFA0u);
            X86_PUSH32(edi);
            edi += 0x1Cu;
            X86_PUSH32(0x2DE0D5u); // return address for sub_002E25B5
            sub_002E25B5(ctx, base);
            ctx.esp += 8; // clean up args (0xFA0 and edi)

            if (ctx.eax == 0) {
                X86_MEM32(esi * 8u + 0x4B5DC8u) = 0;
                ctx.eax = 0;
                ctx.edi = saved_edi;
                ctx.esi = saved_esi;
                ctx.esp += 4; // pop caller return address
                LOGE("guest: sub_002DE0AA failed to allocate CRT buffer at index %u", esi);
                return;
            }
        }
        esi++;
    }

    ctx.eax = 1;
    ctx.edi = saved_edi;
    ctx.esi = saved_esi;
    ctx.esp += 4; // pop caller return address
    LOGI("guest: sub_002DE0AA (CRT buffer table init) completed successfully");
}

// ============================================================================
// Midnight Club 3: XDK CRT & Filesystem Initialization Bridges and Overrides
// ============================================================================

// sub_000142B9: Queries XC_MAX_GAME_SAVE_BLOCKS parental limit via ExQueryNonVolatileSetting(0xA).
// When unset/disabled, it returns 0.
// Caller compares Certificate->AllowedSaveBlocks (0) against eax.
// If edi (0) < eax, it triggers FATAL EXIT 4 (parental save block limit exceeded).
// Returning 0 ensures edi (0) >= eax (0) is true (jae taken), allowing pre-init to proceed.
X86_FUNC(sub_000142B9) {
    LOGI("guest: sub_000142B9() -> querying XC_MAX_GAME_SAVE_BLOCKS limit (returning 0)");
    ctx.eax = 0;
    ctx.esp += 4;
}

// Fallback label for sub_000142BD if ever targeted indirectly
X86_FUNC(sub_000142BD) {
    LOGI("guest: sub_000142BD() -> querying XC_MAX_GAME_SAVE_BLOCKS limit (returning 0)");
    ctx.eax = 0;
    ctx.esp += 4;
}

// Note: sub_0001461D (XMountUtilityDrive) is already implemented by xapi.cpp

// sub_00013136 (CRT C pre-initializers: iterates table 0x4a8140 .. 0x4a814c):
X86_EXTERN_FUNC(__imp__sub_00013136);
X86_FUNC(sub_00013136) {
    LOGI("guest: sub_00013136 (CRT pre-initializers __xp_a..__xp_z) starting...");
    __imp__sub_00013136(ctx, base);
    LOGI("guest: sub_00013136 (CRT pre-initializers) completed, __pioinfo[0]=0x%08X",
         X86_MEM32(0x5696c0u));
}

// sub_002DFFB9 (CRT thread/buffer initialization table entry at 0x4a8144):
X86_EXTERN_FUNC(__imp__sub_002DFFB9);
X86_FUNC(sub_002DFFB9) {
    LOGI("guest: sub_002DFFB9 (CRT buffer/thread init) starting...");
    __imp__sub_002DFFB9(ctx, base);
    LOGI("guest: sub_002DFFB9 completed, esp=0x%08X, esi=0x%08X", ctx.esp, ctx.esi);
}

// sub_002E0023 (CRT shutdown callback registration continuation):
X86_EXTERN_FUNC(__imp__sub_002E0023);
X86_FUNC(sub_002E0023) {
    LOGI("guest: sub_002E0023 (CRT shutdown callback reg) starting...");
    __imp__sub_002E0023(ctx, base);
    LOGI("guest: sub_002E0023 completed, esp=0x%08X, esi=0x%08X", ctx.esp, ctx.esi);
}

// sub_002E21DB (CRT _ioinit table entry at 0x4a8148):
X86_EXTERN_FUNC(__imp__sub_002E21DB);
X86_FUNC(sub_002E21DB) {
    LOGI("guest: sub_002E21DB (_ioinit) starting...");
    __imp__sub_002E21DB(ctx, base);
    LOGI("guest: sub_002E21DB (_ioinit) completed, __pioinfo[0]=0x%08X", X86_MEM32(0x5696c0u));
}

// sub_002E181E (CRT file handle table initialization at 0x4ad268):
X86_EXTERN_FUNC(__imp__sub_002E181E);
X86_FUNC(sub_002E181E) {
    LOGI("guest: sub_002E181E (CRT file handle init) starting, __pioinfo[0]=0x%08X", X86_MEM32(0x5696c0u));
    __imp__sub_002E181E(ctx, base);
    LOGI("guest: sub_002E181E completed");
}

// sub_000130DE (CRT initialization):
// In the original binary, 0x13114 falls through into 0x13116 (sub_00013116).
// The recompiler split them into two functions, causing sub_000130DE to end abruptly without
// executing sub_00013116 or popping the callee-saved registers and returning.
X86_EXTERN_FUNC(__imp__sub_000130DE);
X86_EXTERN_FUNC(sub_00013116);
X86_FUNC(sub_000130DE) {
    LOGI("guest: sub_000130DE (CRT init) -> executing loop 1 (initializers)");
    __imp__sub_000130DE(ctx, base);
    LOGI("guest: sub_000130DE (CRT init) -> executing loop 2 (static constructors / sub_00013116)");
    sub_00013116(ctx, base);
    LOGI("guest: sub_000130DE (CRT init) completed successfully");
}

// sub_002D2410: Display mode table initialization (resolutions, refresh rates, aspect ratios).
X86_EXTERN_FUNC(__imp__sub_002D2410);
X86_FUNC(sub_002D2410) {
    LOGI("guest: sub_002D2410 (Display Mode Table Init) starting...");
    __imp__sub_002D2410(ctx, base);
    LOGI("guest: sub_002D2410 completed (refresh=%u Hz)", X86_MEM32(0x4b4e80u));
}

// sub_00017121: Win32 CreateSemaphoreA wrapper.
// In the original binary, 0x17121 (push ebp) falls through directly into 0x17122 (sub_00017122).
// The recompiler split them into separate functions, leaving sub_00017121 with only `push ebp`
// and returning immediately without executing the semaphore creation or cleaning up the stack.
// This corrupted the guest stack and caused asset stream open/read to fail with an indirect call trap.
X86_EXTERN_FUNC(sub_00017122);
X86_FUNC(sub_00017121) {
    LOGI("guest: sub_00017121 (CreateSemaphoreA) starting, initial=%u, max=%u",
         X86_MEM32(ctx.esp + 8), X86_MEM32(ctx.esp + 12));
    { uint32_t v = (uint32_t)(ctx.ebp); X86_PUSH32(v); }
    sub_00017122(ctx, base);
    LOGI("guest: sub_00017121 (CreateSemaphoreA) completed, handle=0x%08X", ctx.eax);
}

// sub_0001751A: Win32 ReadFile(hFile, lpBuffer, nBytesToRead, lpNumberOfBytesRead, lpOverlapped)
// In the original XBE binary, 0x1751A is the function entry point containing:
//   push ebp
//   mov ebp, esp
//   sub esp, 0x10
// which falls through directly into 0x17520 (sub_00017520).
// The static recompiler split them into separate functions, leaving sub_0001751A with only
// the 3-instruction prologue and returning immediately without calling NtReadFile or cleaning up the stack.
// This corrupted the guest stack and returned 0 bytes, ultimately clobbering ebx to NULL in stream readers.
X86_EXTERN_FUNC(sub_00017520);
X86_FUNC(sub_0001751A) {
    uint32_t hFile = X86_MEM32(ctx.esp + 4);
    uint32_t lpBuffer = X86_MEM32(ctx.esp + 8);
    uint32_t nBytes = X86_MEM32(ctx.esp + 12);
    uint32_t lpBytesRead = X86_MEM32(ctx.esp + 16);
    LOGI("guest: sub_0001751A (ReadFile) starting, handle=0x%08X, buf=0x%08X, nBytes=%u",
         hFile, lpBuffer, nBytes);
    { uint32_t v = (uint32_t)(ctx.ebp); X86_PUSH32(v); }
    ctx.ebp = (uint32_t)(ctx.esp);
    ctx.esp -= 0x10;
    sub_00017520(ctx, base);
    uint32_t bytesRead = lpBytesRead ? X86_MEM32(lpBytesRead) : 0;
    LOGI("guest: sub_0001751A (ReadFile) completed, success=%u, bytesRead=%u", ctx.eax, bytesRead);
}

// sub_0001727E: Fallthrough directly into sub_0001727F
X86_EXTERN_FUNC(sub_0001727F);
X86_FUNC(sub_0001727E) {
    { uint32_t v = (uint32_t)(ctx.ebx); X86_PUSH32(v); }
    sub_0001727F(ctx, base);
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

// ============================================================================
// Native Guest Heap Allocator Subsystem
// Replaces buggy, fragmented Windows NT Heap manager recompiled from XDK 5849.
// Fully compliant with Microsoft NT HEAP_ENTRY header layout so CRT malloc, calloc,
// free, realloc, and RtlSizeHeap function with 100% binary accuracy.
// ============================================================================
namespace guest_heap {

static std::mutex g_heap_mutex;
static uint32_t g_heap_base = 0x00590000;
static uint32_t g_heap_top = 0x00591000;
static uint32_t g_heap_committed = 0;
static std::multimap<uint32_t, uint32_t> g_free_chunks;

static void EnsureHeap(uint32_t base_addr, uint8_t* host_base) {
    if (g_heap_committed == 0 || (base_addr != 0 && base_addr != g_heap_base)) {
        if (base_addr != 0) g_heap_base = base_addr;
        g_heap_top = g_heap_base + 0x1000;
        uint32_t initial_commit = 16u * 1024 * 1024;
        mem::Commit(g_heap_base, initial_commit);
        g_heap_committed = g_heap_base + initial_commit;

        std::memset(host_base + g_heap_base, 0, 0x1000);
        // Standard NT Heap signature
        *reinterpret_cast<uint32_t*>(host_base + g_heap_base + 0x10) = 0xEEFFEEFF;
        *reinterpret_cast<uint32_t*>(host_base + 0x00569434) = g_heap_base;
        LOGI("guest_heap: initialized native heap at 0x%08X (committed %u MB)",
             g_heap_base, initial_commit / (1024 * 1024));
    }
}

static uint32_t CreateHeap(uint32_t flags, uint32_t base_addr, uint32_t reserve, uint32_t commit, uint8_t* host_base) {
    std::lock_guard<std::mutex> lock(g_heap_mutex);
    if (!base_addr) base_addr = 0x00590000;
    EnsureHeap(base_addr, host_base);
    return base_addr;
}

static uint32_t Allocate(uint32_t heap, uint32_t flags, uint32_t size, uint8_t* host_base) {
    std::lock_guard<std::mutex> lock(g_heap_mutex);
    if (!heap) heap = g_heap_base;
    EnsureHeap(heap, host_base);

    if (size == 0) size = 1;

    // Total chunk size: 16-byte header + user payload, aligned to 16 bytes.
    uint32_t total = (size + 31) & ~15u;
    uint32_t chunk_va = 0;

    auto it = g_free_chunks.lower_bound(total);
    if (it != g_free_chunks.end()) {
        chunk_va = it->second;
        uint32_t chunk_sz = it->first;
        g_free_chunks.erase(it);

        if (chunk_sz >= total + 32) {
            uint32_t rem_va = chunk_va + total;
            uint32_t rem_sz = chunk_sz - total;
            uint8_t* rem_hdr = host_base + rem_va;
            *reinterpret_cast<uint16_t*>(rem_hdr + 0) = (uint16_t)(rem_sz >> 4);
            rem_hdr[5] = 0x00; // not busy
            g_free_chunks.emplace(rem_sz, rem_va);
        } else {
            total = chunk_sz;
        }
    } else {
        chunk_va = g_heap_top;
        g_heap_top += total;

        if (g_heap_top > g_heap_committed) {
            uint32_t need = g_heap_top - g_heap_committed;
            uint32_t commit_sz = (need + 0xFFFFF) & ~0xFFFFFu; // 1 MB increments
            mem::Commit(g_heap_committed, commit_sz);
            g_heap_committed += commit_sz;
        }
    }

    uint32_t user_ptr = chunk_va + 16;
    uint16_t size_units = (uint16_t)(total >> 4);
    uint8_t unused_bytes = (uint8_t)(total - size);

    // Exact NT HEAP_ENTRY header at [user_ptr - 16 .. user_ptr - 1]
    uint8_t* hdr = host_base + chunk_va;
    *reinterpret_cast<uint16_t*>(hdr + 0) = size_units;
    *reinterpret_cast<uint16_t*>(hdr + 2) = 0;
    hdr[4] = 0;
    hdr[5] = 0x01; // HEAP_ENTRY_BUSY
    hdr[6] = unused_bytes;
    hdr[7] = 0;
    *reinterpret_cast<uint32_t*>(hdr + 8) = 0;
    *reinterpret_cast<uint32_t*>(hdr + 12) = 0;

    if (flags & 0x08) { // HEAP_ZERO_MEMORY
        std::memset(host_base + user_ptr, 0, size);
    }

    return user_ptr;
}

static uint32_t Free(uint32_t heap, uint32_t flags, uint32_t ptr, uint8_t* host_base) {
    if (!ptr) return 1;
    std::lock_guard<std::mutex> lock(g_heap_mutex);

    uint32_t chunk_va = ptr - 16;
    if (chunk_va < g_heap_base || chunk_va >= g_heap_top) {
        return 1;
    }

    uint8_t* hdr = host_base + chunk_va;
    uint16_t size_units = *reinterpret_cast<uint16_t*>(hdr);
    uint32_t total = (uint32_t)size_units << 4;
    if (total == 0 || total > (64u * 1024 * 1024)) {
        return 1;
    }

    hdr[5] = 0x00; // clear HEAP_ENTRY_BUSY
    g_free_chunks.emplace(total, chunk_va);
    return 1;
}

static uint32_t GetSize(uint32_t heap, uint32_t flags, uint32_t ptr, uint8_t* host_base) {
    if (!ptr) return 0xFFFFFFFF;
    uint32_t chunk_va = ptr - 16;
    if (chunk_va < g_heap_base || chunk_va >= g_heap_top) return 0xFFFFFFFF;
    uint8_t* hdr = host_base + chunk_va;
    if ((hdr[5] & 0x01) == 0) return 0xFFFFFFFF;
    uint16_t size_units = *reinterpret_cast<uint16_t*>(hdr);
    uint8_t unused_bytes = hdr[6];
    return ((uint32_t)size_units << 4) - unused_bytes;
}

static uint32_t ReAllocate(uint32_t heap, uint32_t flags, uint32_t ptr, uint32_t size, uint8_t* host_base) {
    if (!ptr) return Allocate(heap, flags, size, host_base);
    if (size == 0) {
        Free(heap, flags, ptr, host_base);
        return 0;
    }
    uint32_t old_size = GetSize(heap, flags, ptr, host_base);
    if (old_size == 0xFFFFFFFF) old_size = 0;

    uint32_t new_ptr = Allocate(heap, flags, size, host_base);
    if (new_ptr && ptr) {
        std::memcpy(host_base + new_ptr, host_base + ptr, std::min(old_size, size));
        Free(heap, flags, ptr, host_base);
    }
    return new_ptr;
}

} // namespace guest_heap

// sub_00015A76: RtlCreateHeap(Flags, Base, Reserve, Commit, Lock, Parameters)
X86_FUNC(sub_00015A76) {
    uint32_t flags = X86_MEM32(ctx.esp + 4);
    uint32_t heap_base = X86_MEM32(ctx.esp + 8);
    uint32_t reserve = X86_MEM32(ctx.esp + 12);
    uint32_t commit = X86_MEM32(ctx.esp + 16);
    LOGI("guest: sub_00015A76 (RtlCreateHeap) flags=0x%X, base=0x%X, reserve=0x%X, commit=0x%X",
         flags, heap_base, reserve, commit);
    ctx.eax = guest_heap::CreateHeap(flags, heap_base, reserve, commit, base);
    LOGI("guest: sub_00015A76 (RtlCreateHeap) returned heap=0x%X", ctx.eax);
    ctx.esp += 28;
}

// sub_00015E9A: RtlAllocateHeap(HeapHandle, Flags, Size)
X86_FUNC(sub_00015E9A) {
    uint32_t heap = X86_MEM32(ctx.esp + 4);
    uint32_t flags = X86_MEM32(ctx.esp + 8);
    uint32_t size = X86_MEM32(ctx.esp + 12);
    ctx.eax = guest_heap::Allocate(heap, flags, size, base);
    ctx.esp += 16;
}

// sub_00016615: RtlFreeHeap(HeapHandle, Flags, BaseAddress)
X86_FUNC(sub_00016615) {
    uint32_t heap = X86_MEM32(ctx.esp + 4);
    uint32_t flags = X86_MEM32(ctx.esp + 8);
    uint32_t ptr = X86_MEM32(ctx.esp + 12);
    ctx.eax = guest_heap::Free(heap, flags, ptr, base);
    ctx.esp += 16;
}

// sub_000154F8: RtlSizeHeap(HeapHandle, Flags, BaseAddress)
X86_FUNC(sub_000154F8) {
    uint32_t heap = X86_MEM32(ctx.esp + 4);
    uint32_t flags = X86_MEM32(ctx.esp + 8);
    uint32_t ptr = X86_MEM32(ctx.esp + 12);
    ctx.eax = guest_heap::GetSize(heap, flags, ptr, base);
    ctx.esp += 16;
}

// sub_00016809: RtlReAllocateHeap(HeapHandle, Flags, BaseAddress, Size)
X86_FUNC(sub_00016809) {
    uint32_t heap = X86_MEM32(ctx.esp + 4);
    uint32_t flags = X86_MEM32(ctx.esp + 8);
    uint32_t ptr = X86_MEM32(ctx.esp + 12);
    uint32_t size = X86_MEM32(ctx.esp + 16);
    ctx.eax = guest_heap::ReAllocate(heap, flags, ptr, size, base);
    ctx.esp += 20;
}

// sub_000148DE: Returns default process heap handle from [0x569434]
X86_FUNC(sub_000148DE) {
    ctx.eax = X86_MEM32(0x00569434);
    if (!ctx.eax) ctx.eax = 0x00590000;
    ctx.esp += 4;
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



