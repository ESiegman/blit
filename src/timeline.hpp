#pragma once
// Lanes: what a timeline shows row by row, kept small and asked about one
// pixel column at a time. Nothing here knows of windows or of the app (the
// standard library and common.hpp only), so that blit's agent can carry it
// too, as it carries finder.cpp.
//
// A lane is a list of changes: a digital signal or a bus, a real, a string
// or an event. The times of its changes are kept as differences, each a
// varint, in blocks of 256 changes, each block headed by the absolute time
// of its first change. The values are packed side by side, `stride()` bits
// a change (change i's at bit i * stride), in a plane of bits, and in a
// second plane of the same shape once the signal has had an x or a z (as
// VPI's vecval does it: 0 and 1 are 00 and 10, z is 01, x is 11, value bit
// first). A one-bit signal costs one or two bytes a change, a 32-bit bus
// five or six (3.3 bytes a change in all on a 542 MB dump of Verilator's).
// The value at a block's entry is the value of the change before it, read
// in one step, as every change's is.
//
// The changes are those that change something: a value the lane already
// has is not a change (except an event's, each of which is one), and of
// several values at one time the last is the one kept (and none, if it
// is the value from before that time).
//
// Queries are const and cost O(log n) for n changes, whatever the density:
// a binary search of the blocks, then at most 256 varints decoded. Any
// number of threads may ask at once, as long as nothing is being added.
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common.hpp"

namespace blit {

enum class LaneKind : u8 {
    kBits,   // a signal of `width` bits, each 0, 1, x or z
    kReal,   // a double a change
    kText,   // a string a change
    kEvent,  // a moment, with no value
};

// One bit's state, as the two planes give it (value bit, then x/z bit).
enum class Bit : u8 { k0 = 0, k1 = 1, kZ = 2, kX = 3 };

// One pixel column of a lane (Lane::columns): the changes in [start, end).
// The value at the column's start is change first - 1's (none if first is
// 0): with no change inside, the column is that value; with one, an edge
// from it to change first's; with two or more, a busy band.
struct LaneColumn {
    u64 first = 0;
    u64 count = 0;
};

class Lane {
public:
    static constexpr u32 kBlock = 256;  // changes a block

    Lane();
    Lane(LaneKind kind, u32 width);
    ~Lane();
    Lane(Lane&&) noexcept;
    Lane& operator=(Lane&&) noexcept;

    LaneKind kind() const { return kind_; }
    u32 width() const { return width_; }    // bits of a value (kBits); a real is 64, a string or an event 0
    u32 stride() const { return stride_; }  // bits a change takes in the planes (a string's is its number in the lane's list)
    u64 size() const { return count_; }     // changes
    bool has_xz() const { return !xz_.empty(); }
    u64 first_time() const { return blocks_.empty() ? 0 : blocks_[0].t0; }
    u64 last_time() const { return last_t_; }
    // The memory it holds, in bytes.
    size_t bytes() const;

    // --- Adding, in time order (a time before the last is taken as the
    // last). Each returns the bytes of memory the lane took on for it
    // (mostly 0), for the reader's budget.
    //
    // A value of width() bits: words of 64 bits, bit 0 of the value in bit 0
    // of val[0]; xz alongside, or null for none (VPI's encoding, above).
    size_t add_bits(u64 t, const u64* val, const u64* xz);
    size_t add_real(u64 t, double v);
    size_t add_text(u64 t, std::string_view s);
    size_t add_event(u64 t);
    // Gives back what the lists took on beyond their size (the end of a load).
    void shrink();

    // --- Queries.
    // How many changes come before t, and at or before t: O(log n + 256).
    u64 count_before(u64 t) const { return search(t, false); }
    u64 count_through(u64 t) const { return search(t, true); }
    // Change i's time: O(256).
    u64 time(u64 i) const;
    // The change in force at t (the last at or before it), or ~0 if t comes
    // before the first: O(log n + 256).
    u64 at(u64 t) const { return count_through(t) - 1; }
    // The time of the first change after t, and of the last before t; false
    // if there is none (for n and N): O(log n + 256).
    bool next_change(u64 t, u64* out) const;
    bool prev_change(u64 t, u64* out) const;
    // [t0, t1) in n columns, column k covering [t0 + (t1 - t0) * k / n,
    // t0 + (t1 - t0) * (k + 1) / n): out (n of them) gets each one's
    // changes. One search a column, from where the last ended: O(log n +
    // 256) a column at most, less where they are close together. (2000
    // columns: 120 us over a clock's 260,000 changes, 4 us over 2,000, on an
    // M5 Pro.)
    void columns(u64 t0, u64 t1, std::span<LaneColumn> out) const;

    // --- Values of change i (i < size()): O(width / 64).
    Bit bit(u64 i, u32 b) const;
    // n (at most 64) bits from bit `from`: of the value plane, and of the
    // x/z plane (0 where there is none).
    u64 bits(u64 i, u32 from, u32 n) const { return get(bits_, i * stride_ + from, n); }
    u64 xz(u64 i, u32 from, u32 n) const { return xz_.empty() ? 0 : get(xz_, i * stride_ + from, n); }
    double real(u64 i) const;
    std::string_view text(u64 i) const;
    // As text: 0 1 x z for each bit, the first bit the highest; a real in
    // the fewest digits that read back as the same double; a string as it
    // is; an event as "".
    void format(u64 i, std::string* out) const;

private:
    // Reads n bits (at most 64) at bit `at` of a plane.
    static u64 get(const std::vector<u64>& plane, u64 at, u32 n);

    struct Block {
        u64 t0;  // the time of its first change
        u64 at;  // where its varints start in times_ (its first change has none)
    };
    struct Texts;

    u64 search(u64 t, bool through) const;
    size_t add_time(u64 t, bool* same);
    size_t add_value(u64 t, const u64* val, const u64* xz);
    size_t room_bits(u64 words);
    void put(std::vector<u64>& plane, u64 at, u32 n, const u64* src);
    bool equal(u64 i, const u64* val, const u64* xz) const;
    void pop();

    LaneKind kind_ = LaneKind::kBits;
    u32 width_ = 1, stride_ = 1;
    u64 count_ = 0;
    u64 last_t_ = 0;
    std::vector<u8> times_;
    std::vector<Block> blocks_;
    std::vector<u64> bits_, xz_;
    std::unique_ptr<Texts> texts_;
};

}  // namespace blit
