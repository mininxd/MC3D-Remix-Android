// Clean XACT-layer workaround for misidentified DSound GetMixBins stubs in game.cpp:
//   - game.cpp:476 (sub_003C2240 = IDirectSoundBuffer::GetMixBins) and
//     game.cpp:518 (sub_003C22D4 = CDirectSoundStream::GetMixBins) return S_OK without
//     writing to pMixBins, leaving uninitialized stack garbage in caller frames.
//   - Caller #1 (sub_0040AD79 -> sub_0040F566) is sanitized in xact_safety_hooks.cpp.
//   - Caller #2 (sub_0040FB49) is sanitized here before __imp__sub_0040FB49 runs.

#include <atomic>
#include <cstdint>
#include <cstring>

#include "core/log.h"
#include "cpu/x86_context.h"
#include "game/guest_mem_safe.h"
#include "kernel/kernel.h"

namespace {

// sub_0040FB49 allocates sub esp, 0xA8 with DSMIXBINS at [ebp-0x38] = [orig_esp-0xAC].
constexpr uint32_t kFb49StackZeroBytes = 0x100u;

std::atomic<uint64_t> g_set_output_sanitized{0};

}  // namespace

// CXACTVoice::SetOutputVoice(...) - direct caller of sub_003C2240 / sub_003C22D4.
// Zeroes its local stack frame ([esp-0x100 .. esp)) so its stack-allocated DSMIXBINS
// is guaranteed zeroed when game.cpp's stubbed GetMixBins returns S_OK without writing.
extern "C" void __imp__sub_0040FB49(X86Context& ctx, uint8_t* base);
extern "C" void sub_0040FB49(X86Context& ctx, uint8_t* base) {
    if (ctx.esp > kFb49StackZeroBytes &&
        game::safe_mem::CanAccess(ctx.esp - kFb49StackZeroBytes, kFb49StackZeroBytes)) {
        std::memset(kernel::HostPtr(ctx.esp - kFb49StackZeroBytes), 0, kFb49StackZeroBytes);
    }
    const uint64_t n = ++g_set_output_sanitized;
    if (n <= 4u) {
        LOGR("[OK] dsound_xact_hle: sanitized sub_0040FB49 stack frame before GetMixBins");
    }
    __imp__sub_0040FB49(ctx, base);
}
