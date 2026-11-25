#include "game/dsound_stream_bridge.h"

#include <atomic>
#include <cstdint>

#include "core/log.h"
#include "cpu/x86_context.h"
#include "cpu/x86_ops.h"
#include "game/guest_mem_safe.h"
#include "kernel/kernel.h"

// SYM_DirectSoundCreateStream in generated/game/image_symbols.h
extern "C" void sub_003C391A(X86Context& ctx, uint8_t* base);

namespace game {
namespace dsound_bridge {

namespace {

std::atomic<uint64_t> g_streams_created{0};

}  // namespace

void LogBridgeStats() {
    LOGI("dsound_bridge: createdStreams=%llu", static_cast<unsigned long long>(g_streams_created.load()));
}

}  // namespace dsound_bridge
}  // namespace game

// Strong override for B003B: IDirectSound::CreateSoundStream(this, pdssd, ppStream, pUnkOuter)
// XDK stdcall 4 args -> ret 0x10 (esp += 20). Forwards directly to HLE DirectSoundCreateStream(pdssd, ppStream).
X86_FUNC(sub_003C36E9) {
    const uint32_t saved_esp = ctx.esp;
    const uint32_t pdssd = game::safe_mem::Read32(saved_esp + 0x08u, 0u);
    const uint32_t pp_stream = game::safe_mem::Read32(saved_esp + 0x0Cu, 0u);

    if (!pdssd || !pp_stream || !game::safe_mem::CanAccess(pp_stream, 4u)) {
        ctx.eax = 0x80070057u;  // E_INVALIDARG
        ctx.esp = saved_esp + 20u;
        return;
    }

    // Build 2-arg stdcall frame for sub_003C391A(pdssd, ppStream)
    X86_PUSH32(pp_stream);
    X86_PUSH32(pdssd);
    X86_PUSH32(0x003C370Au);
    sub_003C391A(ctx, base);

    const uint32_t hr = ctx.eax;
    const uint32_t created_obj = game::safe_mem::Read32(pp_stream, 0u);
    const uint64_t n = ++game::dsound_bridge::g_streams_created;
    if (n <= 16u || hr != 0u) {
        LOGI("dsound_bridge: IDirectSound::CreateSoundStream #%llu pdssd=0x%08X -> obj=0x%08X hr=0x%08X",
             static_cast<unsigned long long>(n), pdssd, created_obj, hr);
    }

    ctx.esp = saved_esp + 20u;
}
