#include "timeline.hpp"

#include <algorithm>
#include <bit>
#include <charconv>
#include <unordered_map>

namespace blit {

// A string lane's strings, each kept once, numbered in the order they came.
struct Lane::Texts {
    std::unordered_map<std::string, u32> ids;
    std::vector<const std::string*> list;  // (the map's keys: a node's key stays where it is)
    size_t bytes = 0;
};

namespace {

constexpr u64 mask(u32 n) { return n >= 64 ? ~0ull : (1ull << n) - 1; }

// An unsigned LEB128 varint: seven bits a byte, the lowest first, the top
// bit set on every byte but the last.
inline u64 read_varint(const u8*& p) {
    u64 v = *p++;
    if (v < 0x80) return v;
    v &= 0x7f;
    for (u32 shift = 7;; shift += 7) {
        const u64 c = *p++;
        v |= (c & 0x7f) << shift;
        if (c < 0x80 || shift >= 63) return v;
    }
}

// Room for `more` items, in steps that double, so that a list built an
// item at a time is copied O(log n) times. Returns the bytes taken on.
template <class T>
size_t room(std::vector<T>& v, size_t more) {
    if (v.size() + more <= v.capacity()) return 0;
    const size_t before = v.capacity();
    v.reserve(std::max({v.size() + more, before * 2, 64 / sizeof(T)}));
    return (v.capacity() - before) * sizeof(T);
}

}  // namespace

Lane::Lane() = default;
Lane::~Lane() = default;
Lane::Lane(Lane&&) noexcept = default;
Lane& Lane::operator=(Lane&&) noexcept = default;

Lane::Lane(LaneKind kind, u32 width) : kind_(kind) {
    switch (kind) {
        case LaneKind::kBits: width_ = stride_ = std::max<u32>(width, 1); break;
        case LaneKind::kReal: width_ = stride_ = 64; break;
        case LaneKind::kText: width_ = 0, stride_ = 32; break;
        case LaneKind::kEvent: width_ = stride_ = 0; break;
    }
    if (kind == LaneKind::kText) texts_ = std::make_unique<Texts>();
}

size_t Lane::bytes() const {
    size_t n = sizeof(Lane) + times_.capacity() + blocks_.capacity() * sizeof(Block) + (bits_.capacity() + xz_.capacity()) * sizeof(u64);
    if (texts_) n += sizeof(Texts) + texts_->bytes + texts_->list.capacity() * sizeof(void*);
    return n;
}

u64 Lane::get(const std::vector<u64>& plane, u64 at, u32 n) {
    const u64 w = at >> 6;
    const u32 off = static_cast<u32>(at & 63);
    u64 v = plane[w] >> off;
    if (off + n > 64) v |= plane[w + 1] << (64 - off);
    return v & mask(n);
}

void Lane::put(std::vector<u64>& plane, u64 at, u32 n, const u64* src) {
    for (u32 k = 0; k < n; k += 64) {
        const u32 m = std::min<u32>(64, n - k);
        const u64 v = (src ? src[k / 64] : 0) & mask(m);
        const u64 pos = at + k;
        const u64 w = pos >> 6;
        const u32 off = static_cast<u32>(pos & 63);
        plane[w] = (plane[w] & ~(mask(m) << off)) | (v << off);
        if (off + m > 64) {
            const u32 spill = off + m - 64;
            plane[w + 1] = (plane[w + 1] & ~mask(spill)) | (v >> (64 - off));
        }
    }
}

bool Lane::equal(u64 i, const u64* val, const u64* xz) const {
    for (u32 k = 0; k < stride_; k += 64) {
        const u32 m = std::min<u32>(64, stride_ - k);
        if (get(bits_, i * stride_ + k, m) != (val[k / 64] & mask(m))) return false;
        const u64 have = xz_.empty() ? 0 : get(xz_, i * stride_ + k, m);
        if (have != (xz ? xz[k / 64] & mask(m) : 0)) return false;
    }
    return true;
}

size_t Lane::room_bits(u64 words) {
    size_t grew = 0;
    if (bits_.size() < words) {
        grew += room(bits_, words - bits_.size());
        bits_.resize(words);
    }
    if (!xz_.empty() && xz_.size() < words) {
        grew += room(xz_, words - xz_.size());
        xz_.resize(words);
    }
    return grew;
}

// The time of a change to add: a new change's (true), or the last one's.
size_t Lane::add_time(u64 t, bool* same) {
    *same = count_ > 0 && t <= last_t_;
    if (*same) return 0;
    size_t grew = 0;
    if (count_ % kBlock == 0) {
        grew += room(blocks_, 1);
        blocks_.push_back({t, times_.size()});
    } else {
        grew += room(times_, 10);
        u64 d = t - last_t_;
        while (d >= 0x80) {
            times_.push_back(static_cast<u8>(d | 0x80));
            d >>= 7;
        }
        times_.push_back(static_cast<u8>(d));
    }
    last_t_ = t;
    return grew;
}

// The last change goes (a value that, at the same time, went back to the
// one before it).
void Lane::pop() {
    --count_;
    if (count_ % kBlock == 0) {
        blocks_.pop_back();
    } else {
        const Block& b = blocks_[count_ / kBlock];
        const u8* p = times_.data() + b.at;
        for (u64 j = 1; j < count_ % kBlock; ++j) read_varint(p);
        times_.resize(static_cast<size_t>(p - times_.data()));
    }
    last_t_ = count_ ? time(count_ - 1) : 0;
}

size_t Lane::add_bits(u64 t, const u64* val, const u64* xz) { return kind_ == LaneKind::kBits ? add_value(t, val, xz) : 0; }

size_t Lane::add_value(u64 t, const u64* val, const u64* xz) {
    if (xz) {
        bool any = false;
        for (u32 k = 0; k < stride_ && !any; k += 64) any = (xz[k / 64] & mask(std::min<u32>(64, stride_ - k))) != 0;
        if (!any) xz = nullptr;
    }
    if (count_ > 0) {
        if (equal(count_ - 1, val, xz)) return 0;
        if (t <= last_t_ && count_ >= 2 && equal(count_ - 2, val, xz)) {
            pop();
            return 0;
        }
    }
    bool same = false;
    size_t grew = add_time(t, &same);
    const u64 i = same ? count_ - 1 : count_;
    grew += room_bits(((i + 1) * stride_ + 63) / 64);
    if (xz && xz_.empty()) {
        grew += room(xz_, bits_.capacity());
        xz_.assign(bits_.size(), 0);
    }
    put(bits_, i * stride_, stride_, val);
    if (!xz_.empty()) put(xz_, i * stride_, stride_, xz);
    if (!same) ++count_;
    return grew;
}

size_t Lane::add_real(u64 t, double v) {
    if (kind_ != LaneKind::kReal) return 0;
    const u64 bits = std::bit_cast<u64>(v);
    return add_value(t, &bits, nullptr);
}

size_t Lane::add_text(u64 t, std::string_view s) {
    if (kind_ != LaneKind::kText) return 0;
    size_t grew = 0;
    auto [it, fresh] = texts_->ids.try_emplace(std::string(s), static_cast<u32>(texts_->list.size()));
    if (fresh) {
        grew += room(texts_->list, 1);
        texts_->list.push_back(&it->first);
        const size_t b = s.size() + 64;  // (the map's node and the string's own)
        texts_->bytes += b;
        grew += b;
    }
    const u64 id = it->second;
    return grew + add_value(t, &id, nullptr);
}

size_t Lane::add_event(u64 t) {
    if (kind_ != LaneKind::kEvent) return 0;
    bool same = false;
    const size_t grew = add_time(t, &same);
    if (!same) ++count_;
    return grew;
}

void Lane::shrink() {
    times_.shrink_to_fit();
    blocks_.shrink_to_fit();
    bits_.shrink_to_fit();
    xz_.shrink_to_fit();
    if (texts_) texts_->list.shrink_to_fit();
}

u64 Lane::search(u64 t, bool through) const {
    if (count_ == 0) return 0;
    // The last block whose first change is before t (at or before, through).
    const auto it = through ? std::upper_bound(blocks_.begin(), blocks_.end(), t, [](u64 v, const Block& b) { return v < b.t0; })
                            : std::lower_bound(blocks_.begin(), blocks_.end(), t, [](const Block& b, u64 v) { return b.t0 < v; });
    if (it == blocks_.begin()) return 0;
    const u64 b = static_cast<u64>(it - blocks_.begin()) - 1;
    u64 i = b * kBlock;
    const u64 end = std::min(count_, i + kBlock);
    u64 at = blocks_[b].t0;
    const u8* p = times_.data() + blocks_[b].at;
    for (++i; i < end; ++i) {
        at += read_varint(p);
        if (through ? at > t : at >= t) return i;
    }
    return end;
}

u64 Lane::time(u64 i) const {
    const Block& b = blocks_[i / kBlock];
    u64 t = b.t0;
    const u8* p = times_.data() + b.at;
    for (u64 j = i % kBlock; j > 0; --j) t += read_varint(p);
    return t;
}

bool Lane::next_change(u64 t, u64* out) const {
    const u64 i = count_through(t);
    if (i >= count_) return false;
    *out = time(i);
    return true;
}

bool Lane::prev_change(u64 t, u64* out) const {
    const u64 i = count_before(t);
    if (i == 0) return false;
    *out = time(i - 1);
    return true;
}

void Lane::columns(u64 t0, u64 t1, std::span<LaneColumn> out) const {
    const u64 n = out.size();
    if (n == 0) return;
    if (count_ == 0) {
        std::fill(out.begin(), out.end(), LaneColumn{});
        return;
    }
    t1 = std::max(t1, t0);
    // A walk through the changes from left to right: change i (the first not
    // before the last edge asked for) is in block b, at time at, and p is
    // where its block's next varint is.
    u64 b = 0, i = 0, at = blocks_[0].t0;
    const u8* p = times_.data() + blocks_[0].at;
    auto before = [&](u64 edge) {
        if (i >= count_ || at >= edge) return i;
        if (b + 1 < blocks_.size() && blocks_[b + 1].t0 < edge) {
            const auto it = std::lower_bound(blocks_.begin() + static_cast<std::ptrdiff_t>(b) + 1, blocks_.end(), edge,
                                             [](const Block& k, u64 v) { return k.t0 < v; });
            b = static_cast<u64>(it - blocks_.begin()) - 1;
            i = b * kBlock, at = blocks_[b].t0, p = times_.data() + blocks_[b].at;
        }
        for (;;) {
            ++i;
            if (i == count_) return i;
            if (i % kBlock == 0) {  // (the next block's first change, which is not before edge)
                ++b;
                at = blocks_[b].t0, p = times_.data() + blocks_[b].at;
                return i;
            }
            at += read_varint(p);
            if (at >= edge) return i;
        }
    };
    u64 left = before(t0);
    const u64 span = t1 - t0;
    for (u64 k = 0; k < n; ++k) {
        const u64 edge = t0 + static_cast<u64>(static_cast<unsigned __int128>(span) * (k + 1) / n);
        const u64 right = before(edge);
        out[k] = {left, right - left};
        left = right;
    }
}

namespace {

// The walks of same_next and same_prev, given whether change j has the value.
template <class Same>
u64 run_after(u64 i, u64 count, Same same) {
    bool before = true;  // (change j - 1 has it)
    for (u64 j = i + 1; j < count; ++j) {
        const bool s = same(j);
        if (s && !before) return j;
        before = s;
    }
    return ~0ull;
}

template <class Same>
u64 run_before(u64 i, Same same) {
    u64 j = i;
    while (j > 0 && same(j - 1)) --j;  // (the start of the run i is in)
    while (j > 0 && !same(j - 1)) --j;  // (the end of the run before it)
    if (j == 0) return ~0ull;
    --j;
    while (j > 0 && same(j - 1)) --j;
    return j;
}

}  // namespace

u64 Lane::same_next(u64 i, u32 from, u32 n) const { return same_run(i, from, n, true); }
u64 Lane::same_prev(u64 i, u32 from, u32 n) const { return same_run(i, from, n, false); }

u64 Lane::same_run(u64 i, u32 from, u32 n, bool next) const {
    if (kind_ == LaneKind::kEvent || i >= count_ || n == 0 || from >= stride_ || n > stride_ - from) return ~0ull;
    const bool xz = !xz_.empty();
    if (n <= 64) {  // (a word of each plane a change)
        const u64 v = get(bits_, i * stride_ + from, n), q = xz ? get(xz_, i * stride_ + from, n) : 0;
        auto same = [&](u64 j) {
            const u64 at = j * stride_ + from;
            return get(bits_, at, n) == v && (!xz || get(xz_, at, n) == q);
        };
        return next ? run_after(i, count_, same) : run_before(i, same);
    }
    std::vector<u64> v, q;  // (a bus wider than 64 bits: a word at a time, the first that differs ending it)
    for (u32 k = 0; k < n; k += 64) {
        v.push_back(get(bits_, i * stride_ + from + k, std::min<u32>(64, n - k)));
        q.push_back(xz ? get(xz_, i * stride_ + from + k, std::min<u32>(64, n - k)) : 0);
    }
    auto same = [&](u64 j) {
        const u64 at = j * stride_ + from;
        for (u32 k = 0; k < n; k += 64) {
            const u32 m = std::min<u32>(64, n - k);
            if (get(bits_, at + k, m) != v[k / 64] || (xz && get(xz_, at + k, m) != q[k / 64])) return false;
        }
        return true;
    };
    return next ? run_after(i, count_, same) : run_before(i, same);
}

Bit Lane::bit(u64 i, u32 b) const {
    const u64 at = i * stride_ + b;
    const u64 v = get(bits_, at, 1);
    const u64 m = xz_.empty() ? 0 : get(xz_, at, 1);
    return static_cast<Bit>(v | m << 1);
}

double Lane::real(u64 i) const { return std::bit_cast<double>(get(bits_, i * 64, 64)); }

std::string_view Lane::text(u64 i) const {
    if (!texts_) return {};
    return *texts_->list[get(bits_, i * 32, 32)];
}

void Lane::format(u64 i, std::string* out) const {
    out->clear();
    switch (kind_) {
        case LaneKind::kBits:
            out->resize(width_);
            for (u32 b = 0; b < width_; ++b) (*out)[width_ - 1 - b] = "01zx"[static_cast<u32>(bit(i, b))];
            break;
        case LaneKind::kReal: {
            char buf[32];
            const auto r = std::to_chars(buf, buf + sizeof buf, real(i));
            out->assign(buf, r.ptr);
            break;
        }
        case LaneKind::kText: *out = text(i); break;
        case LaneKind::kEvent: break;
    }
}

}  // namespace blit
