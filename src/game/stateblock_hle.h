#pragma once
// Host-side Xbox XDK 5849 StateBlock HLE operating directly on d3d::State() and guest mirrors.

#include <cstdint>

struct X86Context;

namespace game {
namespace stateblock {

// D3DDevice_BeginStateBlock()
uint32_t Begin(X86Context& ctx, uint8_t* base);

// D3DDevice_EndStateBlock(DWORD* pToken)
uint32_t End(X86Context& ctx, uint8_t* base, uint32_t out_token_va);

// D3DDevice_CreateStateBlock(D3DSTATEBLOCKTYPE Type, DWORD* pToken)
uint32_t Create(X86Context& ctx, uint8_t* base, uint32_t type, uint32_t out_token_va);

// D3DDevice_CaptureStateBlock(DWORD Token)
uint32_t Capture(X86Context& ctx, uint8_t* base, uint32_t token);

// D3DDevice_ApplyStateBlock(DWORD Token)
uint32_t Apply(X86Context& ctx, uint8_t* base, uint32_t token);

// D3DDevice_DeleteStateBlock(DWORD Token)
uint32_t Delete(X86Context& ctx, uint8_t* base, uint32_t token);

// Called from ID3DXEffect assignment hook (sub_0037F096) to catch same-value light writes.
bool IsRecording();
void RecordLightTouch(uint32_t light_idx, bool set_light, bool set_enable);

}  // namespace stateblock
}  // namespace game
