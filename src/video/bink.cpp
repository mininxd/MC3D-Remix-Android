#include <cctype>
#include <mutex>
#include <string>
#include <unordered_set>

#include "common/options.h"
#include "core/log.h"
#include "game/guest_mem_safe.h"
#include "xdk/hle.h"

using namespace kernel;

#if HAS_BinkGetSummary
X86_EXTERN_FUNC(GUEST_ORIG(BinkGetSummary));
X86_FUNC(GUEST(BinkGetSummary)) {
    uint32_t bink = R32(ctx.esp + 4);
    if (bink) {
        // 0x134 bytes read, 0x13C read time in ms
        uint64_t scaled = uint64_t(R32(bink + 0x134)) * 1000;
        uint32_t time = R32(bink + 0x13C);
        if (scaled >> 32 > time) W32(bink + 0x13C, uint32_t(scaled >> 32));
    }
    GUEST_ORIG(BinkGetSummary)(ctx, base);
}
#endif

namespace {

#if HAS_BinkWait
constexpr uint32_t kBinkWait = SYM(BinkWait);
std::mutex g_held_lock;
std::unordered_set<uint32_t> g_held;  // movies whose frame was not decoded this game frame
#endif

}  // namespace

#if HAS_BinkDoFrame
X86_EXTERN_FUNC(GUEST_ORIG(BinkDoFrame));
X86_FUNC(GUEST(BinkDoFrame)) {
    uint32_t bink = R32(ctx.esp + 4);
    if (bink) {
        // Prevent division by zero or 32-bit overflow in Bink rate calculation (0x3AD474)
        uint32_t time = R32(bink + 0x2BC);
        if (time == 0) {
            W32(bink + 0x2BC, 1);
        }
        uint64_t scaled = uint64_t(R32(bink + 0x2C0)) * 1000;
        if (time && (scaled >> 32) >= time) {
            W32(bink + 0x2BC, uint32_t((scaled >> 32) + 1));
        }

#if HAS_BinkWait
        if (CallGuest(ctx, kBinkWait, {bink})) {
            std::lock_guard<std::mutex> lock(g_held_lock);
            g_held.insert(bink);
            ctx.eax = 0;
            ctx.esp += 8;
            return;
        }
#endif
    }
    GUEST_ORIG(BinkDoFrame)(ctx, base);
}
#endif

#if HAS_BinkNextFrame
X86_EXTERN_FUNC(GUEST_ORIG(BinkNextFrame));
X86_FUNC(GUEST(BinkNextFrame)) {
    uint32_t bink = R32(ctx.esp + 4);
#if HAS_BinkWait
    {
        std::lock_guard<std::mutex> lock(g_held_lock);
        if (g_held.erase(bink)) {
            ctx.esp += 8;
            return;
        }
    }
#endif
    GUEST_ORIG(BinkNextFrame)(ctx, base);
}
#endif

// Skip boot/logo intro movies when Skip Intros is enabled, while keeping career/story cutscenes intact.
// Signature: __thiscall bool BinkPlayer::Open(const char* name, int arg2) [ret 0x08].
X86_EXTERN_FUNC(__imp__sub_003639C0);
X86_FUNC(sub_003639C0) {
    if (game::Opt().skip_intros) {
        const uint32_t name_va = R32(ctx.esp + 4u);
        if (name_va) {
            std::string name = game::safe_mem::ReadSanitizedCString(name_va, 128);
            for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (name.find("rockstar") != std::string::npos ||
                name.find("sandiego") != std::string::npos ||
                name.find("mc3intro") != std::string::npos ||
                name.find("legal") != std::string::npos ||
                name.find("esrb") != std::string::npos ||
                name.find("rsg") != std::string::npos) {
                LOGI("[OK] bink: skipped intro video '%s'", name.c_str());
                ctx.eax &= 0xFFFFFF00u;  // return false
                ctx.esp += 12u;          // return addr + 0x08 args
                return;
            }
        }
    }
    __imp__sub_003639C0(ctx, base);
}
