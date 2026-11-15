#pragma once
#include "common.hpp"

namespace blit {

inline bool is_cont(u8 b) { return (b & 0xC0) == 0x80; }

struct Decoded {
    u32 cp;
    u32 len;  // bytes consumed, always >= 1
};

// Decodes one code point from [p, p+n), n >= 1. Malformed input yields
// U+FFFD and consumes exactly one byte, so decoding always makes progress.
inline Decoded decode_utf8(const u8* p, size_t n) {
    u8 b0 = p[0];
    if (b0 < 0x80) return {b0, 1};
    if (b0 >= 0xC2 && b0 <= 0xDF) {
        if (n >= 2 && is_cont(p[1])) return {(u32(b0 & 0x1F) << 6) | (p[1] & 0x3F), 2};
    } else if (b0 >= 0xE0 && b0 <= 0xEF) {
        if (n >= 3 && is_cont(p[1]) && is_cont(p[2])) {
            u32 cp = (u32(b0 & 0x0F) << 12) | (u32(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
            if (cp >= 0x800 && !(cp >= 0xD800 && cp <= 0xDFFF)) return {cp, 3};
        }
    } else if (b0 >= 0xF0 && b0 <= 0xF4) {
        if (n >= 4 && is_cont(p[1]) && is_cont(p[2]) && is_cont(p[3])) {
            u32 cp = (u32(b0 & 0x07) << 18) | (u32(p[1] & 0x3F) << 12) |
                     (u32(p[2] & 0x3F) << 6) | (p[3] & 0x3F);
            if (cp >= 0x10000 && cp <= 0x10FFFF) return {cp, 4};
        }
    }
    return {0xFFFD, 1};
}

// Display width in cells: 2 for East Asian wide and emoji ranges, else 1.
inline u32 cp_width(u32 cp) {
    if (cp < 0x1100) return 1;
    return (cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0xA4CF) || (cp >= 0xAC00 && cp <= 0xD7A3) ||
                   (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFE30 && cp <= 0xFE6F) ||
                   (cp >= 0xFF00 && cp <= 0xFF60) || (cp >= 0xFFE0 && cp <= 0xFFE6) ||
                   (cp >= 0x1F300 && cp <= 0x1FAFF) || (cp >= 0x20000 && cp <= 0x3FFFD)
               ? 2
               : 1;
}

}  // namespace blit
