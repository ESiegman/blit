#pragma once
#include <cstddef>
#include <cstdint>
#include <ctime>

namespace blit {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i32 = std::int32_t;
using i64 = std::int64_t;

// A clock that only goes forward, in nanoseconds (without <chrono>, which
// takes a third of a second to compile in every file that has it). On
// macOS CLOCK_MONOTONIC moves in whole microseconds and takes 16 ns; the
// uptime clock moves in nanoseconds and takes 8 (like steady_clock, it
// stands still while the machine sleeps).
inline u64 now_ns() {
#if defined(__APPLE__)
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
#else
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<u64>(ts.tv_sec) * 1'000'000'000ull + static_cast<u64>(ts.tv_nsec);
#endif
}

}  // namespace blit
