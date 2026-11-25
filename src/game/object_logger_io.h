#pragma once
// Buffered file writer and bounded object/texture name registry for object_logs.txt.

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_set>

namespace game {
namespace objlog {
namespace detail {

std::mutex& IoMutex();
void EnsureOpenLocked();
void WriteLineLocked(bool flush_now, const char* fmt, ...);
void WriteFormatted(bool flush_now, const char* fmt, ...);

void RegisterModelLocked(uint32_t obj_va, const char* name);
void RegisterTextureLocked(uint32_t gfx_tex_va, uint32_t d3d_tex_va, uint32_t data_va, const char* name);

std::string LookupModelLocked(uint32_t obj_va, uint32_t vb0_va, uint32_t ib_va);
std::string LookupTextureLocked(uint32_t d3d_tex_va, uint32_t data_va);

bool InsertBoundedLocked(std::unordered_set<uint64_t>& set, uint64_t key, size_t cap);

size_t RegisteredModelCountLocked();
size_t RegisteredTextureCountLocked();

}  // namespace detail
}  // namespace objlog
}  // namespace game
