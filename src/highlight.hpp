#pragma once
// Style runs for a stretch of the document, as the syntax worker produces
// them: a sorted list of marks, each saying "from this byte on, style S".
//
// The worker is always a little behind typing. Until its next result lands,
// edit() keeps the marks lined up with the text by shifting everything after
// the edit. The marks are stored like a gap buffer (those past `split` carry
// one shared pending shift), so an edit next to the previous one costs O(1)
// however many marks follow it.
#include <algorithm>
#include <vector>

#include "common.hpp"

namespace blit {

class Highlights {
public:
    u64 base = 0;  // the marks cover bytes [base, end)
    u64 end = 0;
    std::vector<u32> off;   // mark offsets from base, ascending
    std::vector<u8> style;  // style id from each mark on; 0 = plain text

    bool empty() const { return off.empty(); }
    size_t size() const { return off.size(); }
    void clear() {
        off.clear();
        style.clear();
        base = end = 0;
        split_ = 0;
        shift_ = 0;
    }
    // Call after filling off/style directly.
    void seal() {
        split_ = off.size();
        shift_ = 0;
    }

    // Absolute byte offset of mark i. The stored offset and the pending
    // shift are added as 32-bit values: either may wrap on its own, their
    // sum is the true offset from base.
    u64 at(size_t i) const { return base + static_cast<u32>(off[i] + (i >= split_ ? static_cast<u32>(shift_) : 0u)); }

    // Index of the first mark past `pos`.
    size_t upper(u64 pos) const {
        size_t lo = 0, hi = off.size();
        while (lo < hi) {
            const size_t mid = (lo + hi) / 2;
            if (at(mid) <= pos) lo = mid + 1;
            else hi = mid;
        }
        return lo;
    }

    // Index of the first mark at or past `pos`.
    size_t lower(u64 pos) const {
        size_t lo = 0, hi = off.size();
        while (lo < hi) {
            const size_t mid = (lo + hi) / 2;
            if (at(mid) < pos) lo = mid + 1;
            else hi = mid;
        }
        return lo;
    }

    // The text changed: old_len bytes at `start` became new_len bytes. New
    // text takes the style of what is to its left, so typing at the end of a
    // word keeps the word's colour.
    void edit(u64 start, u64 old_len, u64 new_len) {
        if (off.empty() || start >= end) return;  // nothing here, or after the covered range
        const i64 delta = static_cast<i64>(new_len) - static_cast<i64>(old_len);
        if (start + old_len <= base) {  // entirely before: the whole range moves
            base += static_cast<u64>(delta);
            end += static_cast<u64>(delta);
            return;
        }
        if (start < base || start + old_len > end) {  // straddles an edge: not worth patching
            clear();
            return;
        }
        // Marks before `start` stay; those from `start` on move.
        move_split(lower(start));
        const u64 gone_to = start + old_len;
        for (size_t i = split_; i < off.size() && at(i) < gone_to; ++i)
            off[i] = static_cast<u32>(gone_to - base - static_cast<u64>(shift_));  // inside the removed text: collapse
        shift_ += delta;
        end += static_cast<u64>(delta);
    }

    // True if both describe the same runs over the same range.
    bool same_as(Highlights& other) {
        if (base != other.base || end != other.end || off.size() != other.off.size()) return false;
        move_split(off.size());  // settle the pending shift on both sides, then compare plainly
        other.move_split(other.off.size());
        return off == other.off && style == other.style;
    }

private:
    void move_split(size_t to) {
        const u32 s = static_cast<u32>(shift_);  // two's complement: adding wraps to the right value
        for (; split_ < to; ++split_) off[split_] += s;
        for (; split_ > to; --split_) off[split_ - 1] -= s;
    }

    size_t split_ = 0;  // marks at or past this index are shift_ bytes further on
    i64 shift_ = 0;
};

// Walks the marks alongside the renderer, which asks for styles at ascending
// offsets. `bits` is what to OR into a cell for the current style.
class StyleCursor {
public:
    u64 next = ~0ull;  // where the style changes next
    u32 bits = 0;

    StyleCursor() = default;
    // lut maps a style id to cell bits.
    StyleCursor(const Highlights* h, const u32* lut) : h_(h && !h->empty() ? h : nullptr), lut_(lut) {
        if (h_) next = 0;  // forces a seek on first use
    }
    bool active() const { return h_ != nullptr; }

    // Makes bits/next valid for byte offset `pos`. Offsets must not go down.
    void advance(u64 pos) {
        if (pos < next) return;
        if (fresh_) {
            fresh_ = false;
            seek(pos);
            return;
        }
        step();
        if (pos >= next) seek(pos);  // far ahead: search instead of walking
    }

private:
    void step() {
        if (i_ < h_->size()) {
            bits = lut_[h_->style[i_]];
            ++i_;
            next = i_ < h_->size() ? h_->at(i_) : h_->end;
        } else {
            bits = 0;
            next = ~0ull;
        }
    }
    void seek(u64 pos) {
        if (pos >= h_->end) {
            i_ = h_->size();
            bits = 0;
            next = ~0ull;
            return;
        }
        i_ = h_->upper(pos);  // first mark after pos; the one before it is in effect
        bits = i_ > 0 ? lut_[h_->style[i_ - 1]] : 0;
        next = i_ < h_->size() ? h_->at(i_) : h_->end;
    }

    const Highlights* h_ = nullptr;
    const u32* lut_ = nullptr;
    size_t i_ = 0;  // index of the next mark
    bool fresh_ = true;
};

}  // namespace blit
