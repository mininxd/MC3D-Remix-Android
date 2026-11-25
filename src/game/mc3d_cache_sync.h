#pragma once
// Synchronization and invalidation of Midnight Club 3's internal D3D shadow caches.
// Prevents MC3D from skipping RS/TSS/texture/shader updates after ID3DXEffect ApplyStateBlock.

namespace game {
namespace mc3d_cache {

// Invalidates MC3D's 166-entry RS valid cache (0x00558C58), shader/texture/palette shadows,
// and synchronizes MC3D's 4x32 TSS shadow array (0x005594B8) to the live D3D device state.
void SyncAndInvalidateAll();

}  // namespace mc3d_cache
}  // namespace game
