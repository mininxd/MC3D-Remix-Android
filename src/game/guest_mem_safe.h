#pragma once
// Safe guest memory inspection helpers used by render accuracy and object logging.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include "kernel/kernel.h"
#include "kernel/memory.h"

namespace game {
namespace safe_mem {

inline bool CanAccess(uint32_t va, uint32_t size) {
    if (size == 0) return false;
    if (va < mem::kUserLow) return false;
    const uint64_t end64 = uint64_t(va) + uint64_t(size);
    if (end64 > 0xFFFFFFFFull || !mem::Base()) return false;

    const uint32_t last = static_cast<uint32_t>(end64 - 1u);
    uint32_t page = va & ~(mem::kPage - 1u);
    const uint32_t last_page = last & ~(mem::kPage - 1u);
    while (true) {
        if (!mem::IsCommitted(page)) return false;
        if (page == last_page) break;
        page += mem::kPage;
    }
    return true;
}

inline uint8_t Read8(uint32_t va, uint8_t fallback = 0) {
    if (!CanAccess(va, 1)) return fallback;
    uint8_t v = fallback;
    std::memcpy(&v, kernel::HostPtr(va), 1);
    return v;
}

inline uint16_t Read16(uint32_t va, uint16_t fallback = 0) {
    if (!CanAccess(va, 2)) return fallback;
    return kernel::R16(va);
}

inline uint32_t Read32(uint32_t va, uint32_t fallback = 0) {
    if (!CanAccess(va, 4)) return fallback;
    return kernel::R32(va);
}

inline float ReadF32(uint32_t va, float fallback = 0.0f) {
    if (!CanAccess(va, 4)) return fallback;
    uint32_t raw = kernel::R32(va);
    float f = fallback;
    std::memcpy(&f, &raw, 4);
    return f;
}

inline std::string ReadSanitizedCString(uint32_t va, size_t max_len = 96) {
    std::string out;
    if (va < mem::kUserLow || !mem::Base()) return out;
    out.reserve(32);
    for (size_t i = 0; i < max_len; ++i) {
        const uint32_t cur = va + static_cast<uint32_t>(i);
        if (!mem::IsCommitted(cur)) break;
        const char ch = *static_cast<const char*>(kernel::HostPtr(cur));
        if (ch == '\0') break;
        const unsigned char uch = static_cast<unsigned char>(ch);
        if (uch < 0x20u || uch > 0x7Eu) {
            if (out.empty()) return {};
            out.push_back('?');
        } else if (ch == '"' || ch == '\n' || ch == '\r') {
            out.push_back('_');
        } else {
            out.push_back(ch);
        }
    }
    return out;
}

}  // namespace safe_mem
}  // namespace game
