#pragma once
// Byte scanning primitives used by the buffer and the renderer.
// Written with GCC's portable vector types, so the same code becomes NEON,
// SSE or AVX depending on the target. No per-CPU intrinsics.
#include <algorithm>
#include <bit>
#include <cstring>

#include "common.hpp"

namespace blit {

static_assert(std::endian::native == std::endian::little);

namespace scan {
typedef u8 Vec __attribute__((vector_size(16)));

inline Vec load(const u8* p) {
    Vec v;
    std::memcpy(&v, p, 16);
    return v;
}

// 0xFF in every lane equal to c.
inline Vec eq(const u8* p, u8 c) { return static_cast<Vec>(load(p) == c); }

struct Halves {
    u64 lo, hi;
};
inline Halves halves(Vec v) {
    Halves h;
    std::memcpy(&h, &v, 16);
    return h;
}
}  // namespace scan

// Number of '\n' in [s, s+n).
inline u32 count_nl(const char* s, size_t n) {
    const u8* p = reinterpret_cast<const u8*>(s);
    u32 total = 0;
    while (n >= 64) {
        // Four byte-wide counters per lane; 255 rounds cannot overflow them.
        const size_t rounds = std::min<size_t>(n / 64, 255);
        scan::Vec a0{}, a1{}, a2{}, a3{};
        for (size_t r = 0; r < rounds; ++r, p += 64) {
            a0 -= scan::eq(p, '\n');
            a1 -= scan::eq(p + 16, '\n');
            a2 -= scan::eq(p + 32, '\n');
            a3 -= scan::eq(p + 48, '\n');
        }
        n -= rounds * 64;
        for (int i = 0; i < 16; ++i) total += u32(a0[i]) + a1[i] + a2[i] + a3[i];
    }
    for (; n; --n) total += *p++ == '\n';
    return total;
}

// Index of the k-th (0-based) '\n' in [s, s+n). The caller guarantees it exists.
inline u32 find_nth_nl(const char* s, size_t n, u32 k) {
    const u8* p = reinterpret_cast<const u8*>(s);
    size_t i = 0;
    // Skip 64-byte groups that end before the newline we want.
    for (; i + 64 <= n; i += 64) {
        scan::Vec c = -(scan::eq(p + i, '\n') + scan::eq(p + i + 16, '\n') + scan::eq(p + i + 32, '\n') +
                        scan::eq(p + i + 48, '\n'));
        scan::Halves h = scan::halves(c);
        const u32 cnt = static_cast<u32>(((h.lo + h.hi) * 0x0101010101010101ull) >> 56);
        if (k < cnt) break;
        k -= cnt;
    }
    for (;; ++i) {
        if (p[i] == '\n') {
            if (k == 0) return static_cast<u32>(i);
            --k;
        }
    }
}

// Length of the leading run of printable ASCII (0x20..0x7E) in [p, p+n).
inline size_t ascii_run(const u8* p, size_t n) {
    size_t i = 0;
    for (; i + 16 <= n; i += 16) {
        scan::Vec v = scan::load(p + i) - 0x20;
        scan::Halves bad = scan::halves(static_cast<scan::Vec>(v >= 0x5F));
        if (bad.lo | bad.hi)
            return i + (bad.lo ? std::countr_zero(bad.lo) >> 3 : 8 + (std::countr_zero(bad.hi) >> 3));
    }
    while (i < n && static_cast<u8>(p[i] - 0x20) < 0x5F) ++i;
    return i;
}

}  // namespace blit
