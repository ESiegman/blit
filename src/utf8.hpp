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

// ---------------------------------------------------------------- widths and clusters

// What each character is to the layout, from Unicode 18.0's data, in two
// bits: 0 no width of its own (combining marks, Mn and Me; the zero width
// joiner; variation selectors; skin-tone modifiers; format characters, Cf,
// other than the few that are drawn); 1 one cell; 2 two cells (East Asian
// Wide and Fullwidth, Emoji_Presentation, and unassigned
// Extended_Pictographic, where new emoji will go); 3 one cell, and part of
// the character before it (a spacing mark: Grapheme_Cluster_Break Extend or
// SpacingMark, and not 0 or 2). Code points 0 to 0x3FFFF, then plane 14's
// first 4096, in blocks of 128 (a byte per block says which of the distinct
// blocks it is, and a block is eight words of sixteen two-bit values). Every
// other code point is 1. U+00AD, a soft hyphen, is 1 as well: it shows.
namespace ucd {
inline constexpr u8 kWidthIndex[2080] = {
    0, 0, 0, 0, 0, 0, 1, 0, 0, 2, 0, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
    16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 0, 0, 0, 27, 0, 0, 0, 0, 0, 0, 0, 28, 29,
    30, 31, 32, 0, 33, 34, 35, 36, 37, 38, 0, 39, 0, 0, 0, 0, 40, 41, 0, 0, 0, 0, 42, 43,
    0, 0, 0, 44, 45, 46, 47, 48, 0, 0, 0, 0, 0, 0, 49, 0, 0, 50, 51, 52, 0, 53, 54, 55,
    56, 57, 58, 59, 60, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54,
    54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54,
    54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54,
    54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54,
    54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54,
    54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54,
    54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54,
    54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54,
    54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54,
    54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 61, 0, 0, 62, 63, 0, 0,
    64, 65, 66, 67, 68, 69, 0, 70, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54,
    54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54,
    54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54,
    54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 71,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 54, 54, 54, 54, 72, 0,
    0, 0, 0, 0, 73, 51, 74, 75, 0, 0, 0, 76, 0, 77, 78, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 79, 80, 0, 0, 0, 0, 81, 0, 0, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92,
    93, 94, 0, 95, 96, 97, 98, 0, 99, 0, 100, 101, 102, 103, 104, 0, 105, 106, 107, 108, 0, 109, 110, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 111, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 112, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 113, 114, 0, 0, 0, 0, 0, 0, 0, 115, 116, 54, 54, 54, 54, 54, 54, 54, 54,
    54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54,
    54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54,
    54, 117, 118, 119, 54, 54, 54, 54, 54, 54, 54, 120, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 121,
    54, 54, 122, 54, 54, 123, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 124, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 125, 0, 0, 0, 126, 127, 128, 129, 130, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 131, 132, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    133, 134, 114, 0, 0, 135, 0, 0, 0, 136, 0, 137, 0, 138, 0, 0, 0, 139, 140, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 141, 142, 0, 143, 54, 54, 144, 145, 146, 147, 148, 149, 150, 151, 0, 152,
    153, 154, 155, 54, 156, 54, 0, 0, 54, 54, 54, 54, 54, 54, 54, 157, 54, 54, 54, 54, 54, 54, 54, 54,
    54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54,
    54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54,
    54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54,
    54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54,
    54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54,
    54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54,
    54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54,
    54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54,
    54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54,
    54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54,
    54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54,
    54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54,
    54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54,
    54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54,
    54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54,
    54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54,
    54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54,
    54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54,
    54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54,
    54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54,
    54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 157,
    54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54,
    54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54,
    54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54,
    54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54,
    54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54,
    54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54,
    54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54,
    54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54,
    54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54,
    54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54,
    54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54,
    54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54,
    54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54,
    54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54,
    54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54,
    54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54,
    54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54,
    54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54,
    54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54,
    54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54,
    54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54,
    54, 54, 54, 54, 54, 54, 54, 157, 158, 0, 159, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};
inline constexpr u32 kWidthBlocks[1280] = {
    0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x55555555,
    0x55500015, 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555,
    0x55555555, 0x00000001, 0x00000000, 0x10000000, 0x55501041, 0x55555555, 0x55555555, 0x55555555,
    0x55555555, 0x54400000, 0x55555555, 0x55555555, 0x00155555, 0x00000000, 0x55555555, 0x55555554,
    0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x14000555, 0x50041400, 0x55555555,
    0x55555555, 0x55555551, 0x55555555, 0x00000000, 0x55400000, 0x55555555, 0x55555555, 0x55555555,
    0x55555555, 0x55555555, 0x00000555, 0x55555554, 0x55555555, 0x55555555, 0x00155555, 0x51555500,
    0x55555555, 0x00100555, 0x50010100, 0x55555555, 0x55555555, 0x55015555, 0x55555555, 0x55555555,
    0x55555555, 0x00001555, 0x55555555, 0x55555555, 0x00055555, 0x00000000, 0x00000010, 0x00000000,
    0x555555C0, 0x55555555, 0x55555555, 0xF4C55555, 0xF3FC0003, 0x55550001, 0x55555505, 0x55555555,
    0x555555F1, 0x55555555, 0x55555555, 0xF4555555, 0x53D7D403, 0x5555D555, 0x55555505, 0x45555555,
    0x555555C1, 0x55555555, 0x55555555, 0xF4555555, 0x50141543, 0x55555551, 0x55555555, 0x55555150,
    0x555555C1, 0x55555555, 0x55555555, 0xF4555555, 0x53DC1003, 0x55555555, 0x55555505, 0x00055555,
    0x555555F1, 0x55555555, 0x55555555, 0x34555555, 0x53D7D403, 0x5555C015, 0x55555505, 0x55555555,
    0x55555545, 0x55555555, 0x55555555, 0xF5555555, 0x53F7F57C, 0x5555D555, 0x55555555, 0x55555555,
    0x555554FC, 0x55555555, 0x55555555, 0x04555555, 0x500407FC, 0x55554155, 0x55555505, 0x55555555,
    0x555555F1, 0x55555555, 0x55555555, 0x34555555, 0x50F7C7FF, 0x55557D55, 0x55555505, 0x555555D5,
    0x555555F0, 0x55555555, 0x55555555, 0xF4155555, 0x53F7F403, 0x5555D555, 0x55555505, 0x55555555,
    0x555555F1, 0x55555555, 0x55555555, 0x55555555, 0xD5455555, 0xFFFF440F, 0x55555555, 0x555555F5,
    0x55555555, 0x55555555, 0x55555555, 0x554000D1, 0x40001555, 0x55555555, 0x55555555, 0x55555555,
    0x55555555, 0x55555555, 0x55555555, 0x540000D1, 0x40005555, 0x55555555, 0x55555555, 0x55555555,
    0x55555555, 0x55505555, 0x55555555, 0xF5511155, 0x55555555, 0x55555555, 0x55555555, 0xC0000001,
    0x01550400, 0x00010000, 0x00000000, 0x54000000, 0x55554555, 0x55555555, 0x55555555, 0x55555555,
    0x55555555, 0x55555555, 0x01555555, 0x43C1000C, 0x55555555, 0x0550F555, 0x55555554, 0x55555401,
    0x51554345, 0x51555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555,
    0xAAAAAAAA, 0xAAAAAAAA, 0xAAAAAAAA, 0xAAAAAAAA, 0xAAAAAAAA, 0xAAAAAAAA, 0x55555555, 0x55555555,
    0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x01555555, 0x55555555, 0x55555555,
    0x55555555, 0x55555C05, 0x55555555, 0x55555705, 0x55555555, 0x55555505, 0x55555555, 0x55555505,
    0x55555555, 0x55555555, 0x55555555, 0xF0003055, 0x0003CFFF, 0x51555500, 0x55555555, 0x55555555,
    0x00155555, 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555,
    0x55554155, 0x55555555, 0x55515555, 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555,
    0x55555555, 0x55555555, 0x55FC3FC0, 0x5503FFCF, 0x55555555, 0x55555555, 0x55555555, 0x55555555,
    0x55555555, 0x553C1555, 0x55555555, 0x55555555, 0x55555555, 0x4000CD55, 0xFC000144, 0x1400003F,
    0x55555555, 0x55555555, 0x55555555, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x55555554,
    0x55555700, 0x55555555, 0x55555555, 0xFCC00C55, 0x555557CF, 0x55555555, 0x00155555, 0x55555500,
    0x55555570, 0x55555555, 0x5030F00D, 0x55555555, 0x55555555, 0x55555555, 0x33F0C555, 0x555555F0,
    0x55555555, 0x55555555, 0x00FFFF55, 0x55550F00, 0x55555555, 0x55555555, 0x55555555, 0x55555555,
    0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x00000040, 0x5154000C, 0x5550D455,
    0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00155555, 0x55555555, 0x40055555, 0x55555555, 0x55555555, 0x55555555, 0x00000400, 0x55555555,
    0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x00000000, 0x00000000, 0x55555554,
    0x55555555, 0x55A55555, 0x55695555, 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555,
    0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x56A95555, 0x55555596,
    0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x69555555,
    0x55555555, 0x55555A55, 0x55555555, 0x5555AAAA, 0xAAAA5555, 0x555555AA, 0x55555555, 0x95555555,
    0xAAA55555, 0x55555595, 0x55A55559, 0x69555555, 0x65555A55, 0x55555655, 0x55655555, 0x596559A5,
    0x55A55955, 0x55555555, 0x55565555, 0x55555555, 0x66555555, 0x55559A95, 0x55555555, 0x55555555,
    0x55555555, 0x5555A955, 0x55555555, 0x95555556, 0x55555555, 0x55555555, 0x55555555, 0x55555555,
    0x55555555, 0x56955555, 0x55555555, 0x55555555, 0x55555555, 0x55555956, 0x55555555, 0x55555555,
    0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x15555555, 0x55555550,
    0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x15555555,
    0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x00000000, 0x00000000,
    0xAAAAAAAA, 0xAA9AAAAA, 0xAAAAAAAA, 0xAAAAAAAA, 0xAAAAAAAA, 0xAAAAAAAA, 0xAAAAAAAA, 0x555555AA,
    0xAAAAAAAA, 0xAAAAAAAA, 0xAAAAAAAA, 0xAAAAAAAA, 0xAAAAAAAA, 0xAAAAAAAA, 0xAAAAAAAA, 0xAAAAAAAA,
    0xAAAAAAAA, 0xAAAAAAAA, 0xAAAAAAAA, 0xAAAAAAAA, 0xAAAAAAAA, 0x55555AAA, 0x55555555, 0xAAAAAAAA,
    0xAAAAAAAA, 0xAAAAAAAA, 0xA00AAAAA, 0x6AAAAAAA, 0xAAAAAAA9, 0xAAAAAAAA, 0xAAAAAAAA, 0xAAAAAAAA,
    0xAAAAAAAA, 0xAA816AAA, 0xAAAAAAAA, 0xAAAAAAAA, 0xAAAAAAAA, 0xAAAAAAAA, 0xAAAAAAAA, 0xAAAAAAAA,
    0xAAAAA955, 0xAAAAAAAA, 0xAAAAAAAA, 0xAAAAAAA9, 0xAAAAAAAA, 0xAAAAAAAA, 0xAAAAAAAA, 0xAAAAAAAA,
    0x6AAAAAAA, 0xAAAAAAAA, 0xAAAAAAAA, 0xAAAAAAAA, 0xAAAAAAAA, 0xAAAAAAAA, 0x95555AAA, 0xAAAAAAAA,
    0xAAAAAAAA, 0x6AAAAAAA, 0xAAAAAAAA, 0xAAAAAAAA, 0x5555AAAA, 0xAAAAAAAA, 0xAAAAAAAA, 0xAAAAAAAA,
    0x56AAAAAA, 0xAAAAAAAA, 0xAAAAAAAA, 0xAAAAAAAA, 0x55556AAA, 0x55555555, 0x55555555, 0x55555555,
    0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x15555555, 0x50000040,
    0x55555555, 0x05555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555550,
    0x55154545, 0x55555555, 0x5455C3D5, 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555,
    0x5555555F, 0x55555555, 0x55555555, 0xFFFFFF55, 0x555550FF, 0x55555555, 0x00000000, 0x15555550,
    0x55555555, 0x55555555, 0x50000555, 0x55555555, 0x00001555, 0x555555F0, 0xAAAAAAAA, 0x56AAAAAA,
    0x555555C0, 0x55555555, 0x55555555, 0xF0F00F15, 0x55555557, 0x55555555, 0x55555155, 0x55555555,
    0x55555555, 0x55555555, 0xC0015555, 0x555543C3, 0x5C555515, 0x55555555, 0x55555555, 0x54555555,
    0x55555555, 0x55555555, 0x55555555, 0x05541404, 0x55555551, 0x55555555, 0xF0D55555, 0x55554D55,
    0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x537CF3D5, 0x55555555,
    0xAAAAAAAA, 0xAAAAAAAA, 0x555555AA, 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555,
    0x55555555, 0x45555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555,
    0x00000000, 0x555AAAAA, 0x00000000, 0xAAAAAAAA, 0xAAAAAAAA, 0xAAAAAA6A, 0x55AA6AAA, 0x55555555,
    0xAAAAAAA9, 0xAAAAAAAA, 0xAAAAAAAA, 0xAAAAAAAA, 0xAAAAAAAA, 0xAAAAAAAA, 0x55555556, 0x55555555,
    0x55555555, 0xF5555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55556AAA, 0x55015555,
    0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x51555555,
    0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555554, 0x55555555,
    0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55400555,
    0x00554101, 0x55555555, 0x55555555, 0x15405555, 0x55555555, 0x55555555, 0x55555555, 0x55555555,
    0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55554155, 0x55555555,
    0x55555555, 0x55555555, 0x55550055, 0x55555555, 0x55555555, 0x55555555, 0x50015555, 0x55555555,
    0x55555555, 0x55555555, 0x54155555, 0x55555555, 0x00155555, 0x55555555, 0x55555555, 0x00000000,
    0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x00000555, 0x55555554, 0x55555555, 0x55555555,
    0x55555005, 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555,
    0x55555573, 0x55555555, 0x55555555, 0x00005555, 0x55554000, 0x55555555, 0x55555555, 0x15555414,
    0x55555570, 0x55555555, 0x55555555, 0x5543C03F, 0x55555545, 0x55555555, 0x55555555, 0x55555555,
    0x55555540, 0x55555555, 0x03001555, 0x55555400, 0x55557D55, 0x55555555, 0x55555555, 0x55555515,
    0x55555570, 0x55555555, 0x55555555, 0xC0000FD5, 0x34015557, 0x55555555, 0x55555555, 0x55555555,
    0x55555555, 0x55555555, 0x3F555555, 0x45550CF0, 0x55555551, 0x55555555, 0x55555555, 0x55555555,
    0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x15555555, 0x5540003F, 0x55555555,
    0x555555F0, 0x55555555, 0x55555555, 0xF4155555, 0x5FD7D7FC, 0x5555D555, 0x540005F5, 0x55555400,
    0x55555555, 0x55555555, 0x55555555, 0x003F5555, 0xCF7FDD74, 0x55555544, 0x55555541, 0x55555555,
    0x55555555, 0x55555555, 0x55555555, 0x0000FD55, 0x55554C0F, 0x45555555, 0x55555555, 0x55555555,
    0x55555555, 0x55555555, 0x55555555, 0x3FCC003F, 0x5555550C, 0x55555555, 0x55555555, 0x55555555,
    0x55555555, 0x55555555, 0xD5555555, 0x30FF500F, 0x55555554, 0x50555555, 0x55555555, 0x55555555,
    0x55555555, 0x55555555, 0x55555555, 0x33C0003F, 0x55555554, 0x55555555, 0x55555555, 0x55555555,
    0x55555555, 0x55555555, 0xF3155555, 0x55553000, 0x55555555, 0x55555555, 0x55555555, 0x55555555,
    0x55555555, 0x31555555, 0x55003005, 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555,
    0x55555555, 0x55555555, 0x3F555555, 0x55430000, 0x55555555, 0x55555555, 0x55555555, 0x55555555,
    0x55555555, 0x55555555, 0x55555555, 0x4C17DFFF, 0x55555537, 0x55555555, 0x55555555, 0x55555555,
    0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0xFF0500FD, 0x55555754, 0x55555555,
    0x55400001, 0x55555555, 0x55555555, 0x401C0015, 0x55551555, 0x5503C001, 0x55555555, 0x55555555,
    0x00055555, 0x5550C000, 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555,
    0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x5555CC0C, 0x55555555,
    0x55555555, 0x55555555, 0xD5555555, 0x30004000, 0x55555555, 0x55555555, 0x55555555, 0x55555555,
    0x55555555, 0x00000005, 0x000D0000, 0x5555430C, 0x55555555, 0x55555555, 0x55555555, 0x55555555,
    0x55555555, 0x55555555, 0x55555555, 0x10454001, 0x55551000, 0x55555555, 0x55555555, 0x55555555,
    0x7FF55555, 0x555533D0, 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555554,
    0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55557C15,
    0x555555D0, 0x55555555, 0x55555555, 0xF5400F55, 0x5555554C, 0x55455555, 0x55555555, 0x55555555,
    0x55555555, 0x55555555, 0x55555555, 0x00000000, 0x00001554, 0x55555000, 0x55555555, 0x55555555,
    0x55555555, 0x05555555, 0x03F00000, 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555,
    0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555400,
    0x55555555, 0x55555555, 0x55555555, 0x55554000, 0x55555555, 0x55555555, 0x55555555, 0x55555555,
    0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x15555555, 0xFFFFFFFD, 0xFFFFFFFF, 0xFFFFFFFF,
    0x1555FFFF, 0x55555540, 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x555554AA, 0x55556AAA,
    0xAAAAAAAA, 0xAAAAAAAA, 0xAAAAAAAA, 0xAAAAAAAA, 0xAAAAAAAA, 0x556AAAAA, 0x55555555, 0x95555555,
    0xAAAAAAAA, 0xAAAAAAAA, 0x55555556, 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555,
    0xAAAAAAAA, 0xAAAAAAAA, 0xAAAAAAAA, 0xAAAAAAAA, 0xAAAAAAAA, 0xAAAAAAAA, 0xAAAAAAAA, 0x5555556A,
    0xAAAAAAAA, 0x5555555A, 0xAAAAAAAA, 0xAAAAAAAA, 0xAAAAAAAA, 0x5555556A, 0x55555555, 0x55555555,
    0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x69AAA9AA,
    0xAAAAAAAA, 0xAAAAAAAA, 0x5556AAAA, 0x55555565, 0x55555555, 0x5555596A, 0x5556AA55, 0xAAAAAAAA,
    0xAAAAAAAA, 0xAAAAAAAA, 0xAAAAAAAA, 0xAAAAAAAA, 0xAAAAAAAA, 0xAAAAAAAA, 0xAAAAAAAA, 0x55AAAAAA,
    0x55555555, 0x41555555, 0x55555500, 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555,
    0x00000000, 0x00000000, 0x50000000, 0x00000000, 0x55554000, 0x55555555, 0x55555555, 0x55555555,
    0x55555555, 0x55555555, 0x55541555, 0x55555555, 0x55555555, 0x55555555, 0xFD503D55, 0x0000003F,
    0x55000140, 0x55555555, 0x50055555, 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555,
    0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555405, 0xD415557F, 0x55555555, 0x55555555,
    0x5555555F, 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555,
    0xAAAAAAAA, 0xAAAAAAAA, 0xAAAAAAAA, 0xAAAAAAAA, 0xAAAAAAAA, 0x55556AAA, 0xAAAAAAAA, 0x55556AAA,
    0x00000000, 0x00000000, 0x00000000, 0x00154000, 0x00000000, 0x00000000, 0x54000000, 0x55555155,
    0x55555455, 0x00155555, 0x00000001, 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555,
    0x00004000, 0x00140000, 0x55400410, 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555,
    0x15555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555,
    0x55555555, 0x55555555, 0x45555555, 0x55555555, 0x55555555, 0x55555555, 0x00555555, 0x55555555,
    0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x00555555, 0x55555555,
    0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x05555555, 0x55555555,
    0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x05554515, 0x55555155,
    0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55554000, 0x55555555, 0x55555555,
    0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55400055, 0x55555555, 0x55555555, 0x55555555,
    0x55555655, 0x55555555, 0xAA555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555,
    0x55555555, 0xAAAAAA55, 0x95555555, 0x55555556, 0x95555556, 0x55555556, 0x55555555, 0xAAAAA555,
    0x65555555, 0x556AAAA9, 0xA5555555, 0xAAAAAAAA, 0xAAAAAAAA, 0xAAAAAAAA, 0xAAAAAAAA, 0xAAAAAAAA,
    0xAAAAAAAA, 0xAAAAAAAA, 0xA9555556, 0xAAAA9AAA, 0xAAAAAAAA, 0xAAAAAAAA, 0xAAAAAAAA, 0xA6AAAAAA,
    0xAAAAAAAA, 0x555555AA, 0xAAAAAAAA, 0xAAAAAAAA, 0x956AAAAA, 0x555555AA, 0xAAAAAAAA, 0x002A5656,
    0xAAAAAAAA, 0xAAAAAAAA, 0xAAAAAAAA, 0x6AAAAAAA, 0xAAAAAAA6, 0xAAAAAAAA, 0xAAAAAAAA, 0xAAAAAAAA,
    0xAAAAAAAA, 0xAAAAAAAA, 0xAAAAAAAA, 0xAAAAAAAA, 0xAAAAAAAA, 0xAAAAAAAA, 0xAAAAAAAA, 0x96AAAAAA,
    0xAAAAAAAA, 0xAAAAAAAA, 0xAAAAAAAA, 0x5AAAAAAA, 0x6A955555, 0xAAAAAAAA, 0x5555AAAA, 0x55655555,
    0x55555555, 0x55556955, 0x55555655, 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0xAA955555,
    0xAAAAAAAA, 0xAAAAAAAA, 0xAAAAAAAA, 0xAAAAAAAA, 0xAAAAAAAA, 0x55555555, 0x55555555, 0x55555555,
    0xAAAAAAAA, 0xAAAAAAAA, 0xAAAAAAAA, 0xAAAAAAAA, 0x56555AAA, 0xAAAAA96A, 0xAA955555, 0xAAAAAA55,
    0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0xAA655555, 0xAAAAAAAA, 0x55555556,
    0xAA555555, 0x55555555, 0x55555555, 0x55555555, 0xAAAA5555, 0xAAA55555, 0x55555555, 0x55555555,
    0xAAAA5555, 0x55555555, 0xA5555555, 0xAA555555, 0xAAAAAAA5, 0xAAA95555, 0xAAAAAAAA, 0xAAAAAAAA,
    0xAA555555, 0xAAAAAAAA, 0xAAAAAAAA, 0xAA6AAAAA, 0xAAAA9AAA, 0xAAAAAAAA, 0xAAAAAAAA, 0xAAAAAAAA,
    0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0xAAAA5555, 0xA5555555, 0xAAAAAAAA,
    0xAAAAAAAA, 0xAAAAAAAA, 0xAAAAAAAA, 0xAAAAAAAA, 0xAAAAAAAA, 0xAAAAAAAA, 0xAAAAAAAA, 0x5AAAAAAA,
    0x55555551, 0x55555555, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
};
// Characters of no width that are clusters of their own all the same
// (Grapheme_Cluster_Break Control): a cursor stops on either side of them.
inline constexpr u32 kZeroControls[][2] = {
    {0x061C, 0x061C}, {0x180E, 0x180E}, {0x200B, 0x200B}, {0x200E, 0x200F},
    {0x202A, 0x202E}, {0x2060, 0x2064}, {0x2066, 0x206F}, {0xFEFF, 0xFEFF},
    {0xFFF9, 0xFFFB}, {0x13430, 0x1343F}, {0x1BCA0, 0x1BCA3}, {0x1D173, 0x1D17A},
    {0xE0001, 0xE0001},
};
// Extended_Pictographic: what an emoji sequence joins after a zero width joiner.
inline constexpr u32 kPictographic[][2] = {
    {0x00A9, 0x00A9}, {0x00AE, 0x00AE}, {0x203C, 0x203C}, {0x2049, 0x2049},
    {0x2122, 0x2122}, {0x2139, 0x2139}, {0x2194, 0x2199}, {0x21A9, 0x21AA},
    {0x231A, 0x231B}, {0x2328, 0x2328}, {0x23CF, 0x23CF}, {0x23E9, 0x23F3},
    {0x23F8, 0x23FA}, {0x24C2, 0x24C2}, {0x25AA, 0x25AB}, {0x25B6, 0x25B6},
    {0x25C0, 0x25C0}, {0x25FB, 0x25FE}, {0x2600, 0x2604}, {0x260E, 0x260E},
    {0x2611, 0x2611}, {0x2614, 0x2615}, {0x2618, 0x2618}, {0x261D, 0x261D},
    {0x2620, 0x2620}, {0x2622, 0x2623}, {0x2626, 0x2626}, {0x262A, 0x262A},
    {0x262E, 0x262F}, {0x2638, 0x263A}, {0x2640, 0x2640}, {0x2642, 0x2642},
    {0x2648, 0x2653}, {0x265F, 0x2660}, {0x2663, 0x2663}, {0x2665, 0x2666},
    {0x2668, 0x2668}, {0x267B, 0x267B}, {0x267E, 0x267F}, {0x2692, 0x2697},
    {0x2699, 0x2699}, {0x269B, 0x269C}, {0x26A0, 0x26A1}, {0x26A7, 0x26A7},
    {0x26AA, 0x26AB}, {0x26B0, 0x26B1}, {0x26BD, 0x26BE}, {0x26C4, 0x26C5},
    {0x26C8, 0x26C8}, {0x26CE, 0x26CF}, {0x26D1, 0x26D1}, {0x26D3, 0x26D4},
    {0x26E9, 0x26EA}, {0x26F0, 0x26F5}, {0x26F7, 0x26FA}, {0x26FD, 0x26FD},
    {0x2702, 0x2702}, {0x2705, 0x2705}, {0x2708, 0x270D}, {0x270F, 0x270F},
    {0x2712, 0x2712}, {0x2714, 0x2714}, {0x2716, 0x2716}, {0x271D, 0x271D},
    {0x2721, 0x2721}, {0x2728, 0x2728}, {0x2733, 0x2734}, {0x2744, 0x2744},
    {0x2747, 0x2747}, {0x274C, 0x274C}, {0x274E, 0x274E}, {0x2753, 0x2755},
    {0x2757, 0x2757}, {0x2763, 0x2764}, {0x2795, 0x2797}, {0x27A1, 0x27A1},
    {0x27B0, 0x27B0}, {0x27BF, 0x27BF}, {0x2934, 0x2935}, {0x2B05, 0x2B07},
    {0x2B1B, 0x2B1C}, {0x2B50, 0x2B50}, {0x2B55, 0x2B55}, {0x3030, 0x3030},
    {0x303D, 0x303D}, {0x3297, 0x3297}, {0x3299, 0x3299}, {0x1F004, 0x1F004},
    {0x1F02C, 0x1F02F}, {0x1F094, 0x1F09F}, {0x1F0AF, 0x1F0B0}, {0x1F0C0, 0x1F0C0},
    {0x1F0CF, 0x1F0D0}, {0x1F0F6, 0x1F0FF}, {0x1F170, 0x1F171}, {0x1F17E, 0x1F17F},
    {0x1F18E, 0x1F18E}, {0x1F191, 0x1F19A}, {0x1F1AF, 0x1F1E5}, {0x1F201, 0x1F20F},
    {0x1F21A, 0x1F21A}, {0x1F22F, 0x1F22F}, {0x1F232, 0x1F23A}, {0x1F23C, 0x1F23F},
    {0x1F249, 0x1F25F}, {0x1F266, 0x1F321}, {0x1F324, 0x1F393}, {0x1F396, 0x1F397},
    {0x1F399, 0x1F39B}, {0x1F39E, 0x1F3F0}, {0x1F3F3, 0x1F3F5}, {0x1F3F7, 0x1F3FA},
    {0x1F400, 0x1F4FD}, {0x1F4FF, 0x1F53D}, {0x1F549, 0x1F54E}, {0x1F550, 0x1F567},
    {0x1F56F, 0x1F570}, {0x1F573, 0x1F57A}, {0x1F587, 0x1F587}, {0x1F58A, 0x1F58D},
    {0x1F590, 0x1F590}, {0x1F595, 0x1F596}, {0x1F5A4, 0x1F5A5}, {0x1F5A8, 0x1F5A8},
    {0x1F5B1, 0x1F5B2}, {0x1F5BC, 0x1F5BC}, {0x1F5C2, 0x1F5C4}, {0x1F5D1, 0x1F5D3},
    {0x1F5DC, 0x1F5DE}, {0x1F5E1, 0x1F5E1}, {0x1F5E3, 0x1F5E3}, {0x1F5E8, 0x1F5E8},
    {0x1F5EF, 0x1F5EF}, {0x1F5F3, 0x1F5F3}, {0x1F5FA, 0x1F64F}, {0x1F680, 0x1F6C5},
    {0x1F6CB, 0x1F6D2}, {0x1F6D5, 0x1F6E5}, {0x1F6E9, 0x1F6E9}, {0x1F6EB, 0x1F6F0},
    {0x1F6F3, 0x1F6FF}, {0x1F7DC, 0x1F7F0}, {0x1F80C, 0x1F80F}, {0x1F848, 0x1F84F},
    {0x1F85A, 0x1F85F}, {0x1F888, 0x1F88F}, {0x1F8AE, 0x1F8AF}, {0x1F8BC, 0x1F8BF},
    {0x1F8C2, 0x1F8CF}, {0x1F8D9, 0x1F8FF}, {0x1F90C, 0x1F93A}, {0x1F93C, 0x1F945},
    {0x1F947, 0x1F9FF}, {0x1FA58, 0x1FA5F}, {0x1FA6E, 0x1FAFF}, {0x1FC00, 0x1FFFD},
};

// Grapheme_Cluster_Break Prepend (Unicode 18.0): a sign that goes with the
// character after it, as an Arabic number sign goes with its digits.
inline constexpr u32 kPrepend[][2] = {
    {0x0600, 0x0605}, {0x06DD, 0x06DD}, {0x070F, 0x070F}, {0x0890, 0x0891}, {0x08E2, 0x08E2},
    {0x0D4E, 0x0D4E}, {0x110BD, 0x110BD}, {0x110CD, 0x110CD}, {0x111C2, 0x111C3},
    {0x113D1, 0x113D1}, {0x1193F, 0x1193F}, {0x11941, 0x11941}, {0x11A84, 0x11A89},
    {0x11D46, 0x11D46}, {0x11F02, 0x11F02},
};
// Indic_Conjunct_Break Consonant and Linker (Unicode 18.0, from
// DerivedCoreProperties): a consonant, a linker (a virama) and a consonant
// are one character, a conjunct, as in Devanagari's "क्ष".
inline constexpr u32 kConsonant[][2] = {
    {0x0915, 0x0939}, {0x0958, 0x095F}, {0x0978, 0x097F}, {0x0995, 0x09A8}, {0x09AA, 0x09B0},
    {0x09B2, 0x09B2}, {0x09B6, 0x09B9}, {0x09DC, 0x09DD}, {0x09DF, 0x09DF}, {0x09F0, 0x09F1},
    {0x0A95, 0x0AA8}, {0x0AAA, 0x0AB0}, {0x0AB2, 0x0AB3}, {0x0AB5, 0x0AB9}, {0x0AF9, 0x0AF9},
    {0x0B15, 0x0B28}, {0x0B2A, 0x0B30}, {0x0B32, 0x0B33}, {0x0B35, 0x0B39}, {0x0B5C, 0x0B5D},
    {0x0B5F, 0x0B5F}, {0x0B71, 0x0B71}, {0x0C15, 0x0C28}, {0x0C2A, 0x0C39}, {0x0C58, 0x0C5A},
    {0x0D15, 0x0D3A}, {0x1000, 0x102A}, {0x103F, 0x103F}, {0x1050, 0x1055}, {0x105A, 0x105D},
    {0x1061, 0x1061}, {0x1065, 0x1066}, {0x106E, 0x1070}, {0x1075, 0x1081}, {0x108E, 0x108E},
    {0x1780, 0x17B3}, {0x1A20, 0x1A54}, {0x1B0B, 0x1B0C}, {0x1B13, 0x1B33}, {0x1B45, 0x1B4C},
    {0x1B83, 0x1BA0}, {0x1BAE, 0x1BAF}, {0x1BBB, 0x1BBD}, {0xA989, 0xA98B}, {0xA98F, 0xA9B2},
    {0xA9E0, 0xA9E4}, {0xA9E7, 0xA9EF}, {0xA9FA, 0xA9FE}, {0xAA60, 0xAA6F}, {0xAA71, 0xAA73},
    {0xAA7A, 0xAA7A}, {0xAA7E, 0xAA7F}, {0xAAE0, 0xAAEA}, {0xABC0, 0xABDA}, {0x10A00, 0x10A00},
    {0x10A10, 0x10A13}, {0x10A15, 0x10A17}, {0x10A19, 0x10A35}, {0x11103, 0x11126},
    {0x11144, 0x11144}, {0x11147, 0x11147}, {0x11380, 0x11389}, {0x1138B, 0x1138B},
    {0x1138E, 0x1138E}, {0x11390, 0x113B5}, {0x11900, 0x11906}, {0x11909, 0x11909},
    {0x1190C, 0x11913}, {0x11915, 0x11916}, {0x11918, 0x1192F}, {0x11A00, 0x11A00},
    {0x11A0B, 0x11A32}, {0x11A50, 0x11A50}, {0x11A5C, 0x11A83}, {0x11B0A, 0x11B0A},
    {0x11DF1, 0x11DF1}, {0x11F04, 0x11F10}, {0x11F12, 0x11F33},
};
inline constexpr u32 kLinker[][2] = {
    {0x094D, 0x094D}, {0x09CD, 0x09CD}, {0x0ACD, 0x0ACD}, {0x0B4D, 0x0B4D}, {0x0C4D, 0x0C4D},
    {0x0D4D, 0x0D4D}, {0x1039, 0x1039}, {0x17D2, 0x17D2}, {0x1A60, 0x1A60}, {0x1B44, 0x1B44},
    {0x1BAB, 0x1BAB}, {0x1CF5, 0x1CF6}, {0xA9C0, 0xA9C0}, {0xAAF6, 0xAAF6}, {0x10A3F, 0x10A3F},
    {0x11133, 0x11133}, {0x113D0, 0x113D0}, {0x1193E, 0x1193E}, {0x11A3A, 0x11A3A},
    {0x11A47, 0x11A47}, {0x11A99, 0x11A99}, {0x11F42, 0x11F42},
};

template <size_t N>
bool in_ranges(const u32 (&r)[N][2], u32 cp) {
    size_t lo = 0, hi = N;
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (cp > r[mid][1]) lo = mid + 1;
        else hi = mid;
    }
    return lo < N && cp >= r[lo][0];
}
}  // namespace ucd

// The two bits above, for any code point.
inline u32 cp_class(u32 cp) {
    if (cp >= 0x40000) {
        if (cp - 0xE0000 >= 0x1000) return 1;
        cp = cp - 0xE0000 + 0x40000;  // (plane 14's block follows plane 3's in the table)
    }
    return ucd::kWidthBlocks[ucd::kWidthIndex[cp >> 7] * 8u + ((cp >> 4) & 7)] >> ((cp & 15) * 2) & 3;
}

// Display width in cells: 0, 1, or 2. Everything below U+0300 (ASCII,
// Latin-1 and the extended Latin letters) is 1 without looking anything up.
inline u32 cp_width(u32 cp) {
    if (cp < 0x300) return 1;
    const u32 c = cp_class(cp);
    return c == 3 ? 1 : c;
}

// True when the character belongs to the grapheme cluster of the one before
// it: a combining or spacing mark, a joiner, a variation selector, a
// skin-tone modifier. (The cluster rules that look further back are Joiner's.)
inline bool cp_extends(u32 cp) {
    if (cp < 0x300) return false;
    const u32 c = cp_class(cp);
    return c == 3 || (c == 0 && !ucd::in_ranges(ucd::kZeroControls, cp));
}

inline bool cp_pictographic(u32 cp) { return cp >= 0xA9 && ucd::in_ranges(ucd::kPictographic, cp); }
inline bool cp_regional(u32 cp) { return cp - 0x1F1E6 < 26; }  // a regional indicator: two of them are a flag

// Hangul's jamo and syllables, as the cluster rules have them
// (Grapheme_Cluster_Break L, V, T, LV and LVT).
enum : u8 { kJamoNone, kJamoL, kJamoV, kJamoT, kJamoLV, kJamoLVT };
inline u8 hangul_kind(u32 cp) {
    if (cp - 0x1100 < 0x60 || cp - 0xA960 < 0x1D) return kJamoL;
    if (cp - 0x1160 < 0x48 || cp - 0xD7B0 < 0x17 || cp == 0x16D63 || cp - 0x16D67 < 4) return kJamoV;
    if (cp - 0x11A8 < 0x58 || cp - 0xD7CB < 0x31) return kJamoT;
    if (cp - 0xAC00 < 11172) return (cp - 0xAC00) % 28 == 0 ? kJamoLV : kJamoLVT;
    return kJamoNone;
}
// Whether a jamo or syllable of kind b goes on with one of kind a (a
// syllable written in its parts: a leading consonant, a vowel, a trailing
// consonant).
inline bool hangul_joins(u8 a, u8 b) {
    if (a == kJamoL) return b == kJamoL || b == kJamoV || b == kJamoLV || b == kJamoLVT;
    if (a == kJamoV || a == kJamoLV) return b == kJamoV || b == kJamoT;
    if (a == kJamoT || a == kJamoLVT) return b == kJamoT;
    return false;
}

// A prepended sign (Grapheme_Cluster_Break Prepend).
inline bool cp_prepend(u32 cp) {
    if (cp < 0x10000) return cp - 0x600 < 6 || cp == 0x6DD || cp == 0x70F || cp - 0x890 < 2 || cp == 0x8E2 || cp == 0xD4E;
    return cp - 0x110BD < 0x11F03 - 0x110BD && ucd::in_ranges(ucd::kPrepend, cp);
}
// Whether cp is in a block that has conjunct consonants or linkers (the
// tables need looking at only there).
inline bool cp_conjunct_block(u32 cp) { return cp - 0x900 < 0x1D00 - 0x900 || cp - 0xA980 < 0xAC00 - 0xA980 || cp - 0x10A00 < 0x11F43 - 0x10A00; }

// How a character goes on with the cluster before it, by the rules Joiner
// keeps: not at all; joined with no width of its own (an emoji after a
// zero width joiner, a flag's second half: the cluster is drawn as its
// first); or joined keeping its width (a syllable's jamo, a conjunct's
// next consonant, what follows a prepended sign: each is drawn in its own
// cells, and only where the cursor stops changes).
enum : u8 { kJoinNone, kJoinZeroWidth, kJoinKeepsWidth };

// The cluster rules that look past the character before: an emoji after a
// zero width joiner joins the one before it (a family, a profession: one
// cluster, drawn as its first emoji), regional indicators pair up into
// flags, Hangul's jamo make syllables, a consonant after a linker (a
// virama) makes a conjunct with the consonant before it, and a prepended
// sign goes with what follows. Fed a line's characters in order (or
// reset() where one is ASCII).
struct Joiner {
    u8 after = 0;  // 1: the last character was a joiner; 2: a regional indicator that has no pair yet
    // The rarer rules' state, in one byte (so that it stays in a register
    // through a line's loop): the last character's Hangul kind (bits 0 to
    // 2), a conjunct under way (bits 3 and 4: 1 a consonant, with zero-width
    // marks after it; 2 and a linker since), a prepended sign (bit 5).
    u8 rare = 0;

    void reset() { after = rare = 0; }
    // How cp goes on with the cluster before it (kJoin*; 0, false, when it starts one).
    [[gnu::always_inline]] u8 joins(u32 cp) {
        const u8 was = after;
        after = cp == 0x200D ? 1 : cp_regional(cp) && was != 2 ? 2 : 0;
        u8 how = kJoinNone;
        if (was) [[unlikely]] how = (was == 1 ? cp_pictographic(cp) : cp_regional(cp)) ? kJoinZeroWidth : kJoinNone;
        // Outside the blocks of the scripts these rules are for (Latin,
        // Greek, Cyrillic, Hebrew, CJK, symbols, emoji) nothing is looked
        // up, and nothing is done unless one of them came just before.
        if (rare_block(cp)) [[unlikely]] {
            // A Hangul syllable written whole, after anything but a leading
            // jamo, a conjunct or a prepended sign (Korean text, nearly
            // always): it starts a cluster, and is all there is to keep.
            if (cp - 0xAC00 < 11172 && rare <= kJamoLVT && rare != kJamoL) {
                rare = (cp - 0xAC00) % 28 == 0 ? kJamoLV : kJamoLVT;
                return how;
            }
            const u32 r = rare_step(rare, cp, how);
            rare = static_cast<u8>(r >> 8);
            return static_cast<u8>(r);
        }
        if (rare) [[unlikely]] rare = rare & kConjunctBits ? rare_leave(rare, cp) : 0;
        return how;
    }
    // The columns cp adds to a line: none for one joined to the emoji or
    // flag before it. Only those two rules change a width, so only they are
    // looked at (widths are counted a whole file at a time: the rarer rules
    // would cost a third of that, and change nothing there).
    [[gnu::always_inline]] u32 width(u32 cp) {
        const u8 was = after;
        after = cp == 0x200D ? 1 : cp_regional(cp) && was != 2 ? 2 : 0;
        if (!was) [[likely]] return cp_width(cp);
        return (was == 1 ? cp_pictographic(cp) : cp_regional(cp)) ? 0 : cp_width(cp);
    }
    // Whether cp goes on with the cluster before it, by any rule.
    [[gnu::always_inline]] bool extends(u32 cp) { return joins(cp) != kJoinNone || cp_extends(cp); }

private:
    static constexpr u8 kJamoBits = 7, kConjunctShift = 3, kConjunctBits = 3 << kConjunctShift, kPrependBit = 1 << 5;
    // (A bit for each 4096 code points that has any of them: one test for
    // most text, Latin's below U+0600 and CJK's among them.)
    static constexpr u64 kRareBlocks = 1ull << 0x0 | 1ull << 0x1 | 1ull << 0xA | 1ull << 0xB | 1ull << 0xC | 1ull << 0xD |
                                       1ull << 0x10 | 1ull << 0x11 | 1ull << 0x16;
    static bool rare_block(u32 cp) {
        if (cp < 0x600 || cp >= 0x17000 || !(kRareBlocks >> (cp >> 12) & 1)) [[likely]] return false;
        return cp - 0x600 < 0x1D00 - 0x600 || cp - 0xA960 < 0xD7FC - 0xA960 || cp - 0x10A00 < 0x11F43 - 0x10A00 || cp - 0x16D63 < 8;
    }
    // A character of none of those scripts after one of them: only a
    // conjunct goes on, through zero-width marks and joiners.
    [[gnu::noinline]] static u8 rare_leave(u8 r, u32 cp) {
        return (r & kConjunctBits) && cp_extends(cp) && cp_width(cp) == 0 ? r & kConjunctBits : 0;
    }
    // A character of one of them: the new state (above bit 8) and how cp joins.
    [[gnu::noinline]] static u32 rare_step(u8 r, u32 cp, u8 how) {
        if ((r & kPrependBit) && how == kJoinNone) how = kJoinKeepsWidth;
        const u8 a = r & kJamoBits, jamo = hangul_kind(cp);
        if (a && jamo && hangul_joins(a, jamo) && how == kJoinNone) how = kJoinKeepsWidth;
        u8 conjunct = (r & kConjunctBits) >> kConjunctShift;
        bool decided = false;
        if (cp_conjunct_block(cp)) {
            if (ucd::in_ranges(ucd::kConsonant, cp)) {
                if (conjunct == 2 && how == kJoinNone) how = kJoinKeepsWidth;
                conjunct = 1;
                decided = true;
            } else if (ucd::in_ranges(ucd::kLinker, cp)) {
                if (conjunct) conjunct = 2;
                decided = true;
            }
        }
        if (!decided && conjunct && !(cp_extends(cp) && cp_width(cp) == 0)) conjunct = 0;
        const u8 next = static_cast<u8>(jamo | conjunct << kConjunctShift | (cp_prepend(cp) ? kPrependBit : 0));
        return static_cast<u32>(next) << 8 | how;
    }
};

}  // namespace blit
