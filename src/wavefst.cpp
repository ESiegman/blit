// FST files read into lanes through libfst (third_party/libfst, MIT, at a
// pinned commit): its header, hierarchy and value changes, as
// doc/block_format.txt in libfst describes them, into the same Wave a VCD
// dump is read into (wave.hpp).
#include "wave.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <bit>
#include <cerrno>
#include <charconv>
#include <csetjmp>
#include <cstring>
#include <mutex>

#include "qos.hpp"

extern "C" {
#include "fstapi.h"
// Where libfst's own checks go back to on this thread, instead of ending
// the process (third_party/libfst/config.h); null: nowhere. And the most it
// may allocate at once.
void blit_fst_catch(std::jmp_buf* to, size_t most);
}

namespace blit {

namespace {

constexpr size_t kPiece = 1 << 20;  // the most changes' bytes added under one hold of the Wave's lock
constexpr u32 kMaxWidth = 1 << 20;  // bits of a variable (a wider one keeps its lowest), as for VCD
constexpr size_t kMaxNotes = 50;

// FST's block types (fstapi.h's FST_BL_*), and the header block's length.
enum : u8 { kHdr = 0, kVcData = 1, kHier = 4, kVcDynAlias = 5, kHierLz4 = 6, kHierLz4Duo = 7, kVcDynAlias2 = 8, kZWrapper = 254, kSkip = 255 };
constexpr u64 kHdrLength = 329;

// FST's variable types (FST_VT_*, 0 to 29) as blit's kinds.
constexpr VarKind kVarKinds[] = {
    VarKind::kEvent,   VarKind::kInteger,  VarKind::kParameter, VarKind::kReal,     VarKind::kParameter, VarKind::kReg,
    VarKind::kSupply0, VarKind::kSupply1,  VarKind::kTime,      VarKind::kTri,      VarKind::kTriAnd,    VarKind::kTriOr,
    VarKind::kTriReg,  VarKind::kTri0,     VarKind::kTri1,      VarKind::kWAnd,     VarKind::kWire,      VarKind::kWOr,
    VarKind::kPort,    VarKind::kOther,    VarKind::kRealTime,  VarKind::kString,   VarKind::kBit,       VarKind::kLogic,
    VarKind::kInt,     VarKind::kShortInt, VarKind::kLongInt,   VarKind::kByte,     VarKind::kEnum,      VarKind::kShortReal,
};
static_assert(std::size(kVarKinds) == FST_VT_MAX + 1);

// A value's character as VPI's (value, x/z) bits: 0 1 z x, and the other
// states of std_logic and FST's (h l as 1 0; u w - ? as x).
struct Digits {
    u8 v[256] = {};
    constexpr Digits() {
        for (u8& d : v) d = 3;
        v['0'] = v['l'] = v['L'] = 0;
        v['1'] = v['h'] = v['H'] = 1;
        v['z'] = v['Z'] = 2;
    }
};
constexpr Digits kDigit;

u64 be64(const u8* p) {
    u64 v = 0;
    for (int i = 0; i < 8; ++i) v = v << 8 | p[i];
    return v;
}

bool parse_i32(std::string_view s, i32* out) {
    if (s.empty()) return false;
    const auto r = std::from_chars(s.data(), s.data() + s.size(), *out);
    return r.ec == std::errc() && r.ptr == s.data() + s.size();
}

// "[7:0]" or "[3]".
bool parse_range(std::string_view s, i32* msb, i32* lsb) {
    if (s.size() < 3 || s.front() != '[' || s.back() != ']') return false;
    s = s.substr(1, s.size() - 2);
    const size_t colon = s.find(':');
    if (colon == std::string_view::npos) {
        if (!parse_i32(s, msb)) return false;
        *lsb = *msb;
        return true;
    }
    return parse_i32(s.substr(0, colon), msb) && parse_i32(s.substr(colon + 1), lsb);
}

// Calls f, which calls into libfst, so that one of libfst's checks that
// would end the process, or what a damaged file does to it (config.h there
// says what), comes back here instead: false then. `most` is the most it
// may allocate at once. (f's own frame holds nothing that needs
// destroying, so that jumping out of it is sound.)
template <class F>
bool guarded(size_t most, F&& f) {
    std::jmp_buf jump;
    blit_fst_catch(&jump, most);
    if (setjmp(jump) != 0) {
        blit_fst_catch(nullptr, 0);
        return false;
    }
    f();
    blit_fst_catch(nullptr, 0);
    return true;
}

// What a walk over the file's blocks finds, before libfst reads it: where
// each section of value changes starts and the time it ends at (for
// progress), whether there is a hierarchy, and whether the file is whole.
struct Blocks {
    std::vector<std::pair<u64, u64>> sections;  // (end time, offset)
    bool hier = false;
    std::string wrong;  // why the file cannot be read, if it cannot
};

Blocks walk(int fd, u64 size) {
    Blocks b;
    if (size < 9) {
        b.wrong = "not an FST file: it is too short";
        return b;
    }
    u8 h[25];
    for (u64 off = 0; off < size;) {
        const ssize_t n = ::pread(fd, h, sizeof h, static_cast<off_t>(off));
        if (n < 9) {
            b.wrong = off ? "the FST file is cut short: it ends inside a block's heading, at byte " + std::to_string(size)
                          : "not an FST file: it is too short";
            return b;
        }
        const u8 type = h[0];
        const u64 len = be64(h + 1);
        if (off == 0 && type == kZWrapper) {
            // (gzip around the whole file: libfst unwraps it; not walked.)
            b.hier = true;
            return b;
        }
        if (off == 0 && (type != kHdr || len != kHdrLength)) {
            b.wrong = "not an FST file: it does not start with an FST header";
            return b;
        }
        if (type == kSkip) {
            b.wrong = "the FST file is not finished: its writer is still writing it (or stopped before closing it)";
            return b;
        }
        if (len < 8 || len > size - off - 1) {
            b.wrong = "the FST file is cut short: its block at byte " + std::to_string(off) + " goes past its end, at byte " + std::to_string(size);
            return b;
        }
        if ((type == kVcData || type == kVcDynAlias || type == kVcDynAlias2) && n >= 25) b.sections.push_back({be64(h + 17), off});
        if (type == kHier || type == kHierLz4 || type == kHierLz4Duo) b.hier = true;
        off += 1 + len;
    }
    return b;
}

// One change, kept until the piece it is in is added: the value of up to
// 64 bits (a, b: value and x/z planes), a real's bits (a), or where its
// bytes are in the piece's arena (a, n: a wider value or a string).
struct Change {
    u64 t;
    u32 lane;
    u32 n;
    u64 a, b;
};

struct Fst {
    Wave* w = nullptr;
    u64 max = 0, mem = 0;
    std::stop_token stop;
    const std::function<void(u64)>* progress = nullptr;
    fstReaderContext* ctx = nullptr;
    Blocks blocks;
    u64 size = 0;
    // A handle's (from 1) lane and the length of its values in bytes (its
    // bits, a real's 8, a string's 0: each says its own).
    std::vector<u32> lane_of;
    std::vector<u32> length_of;
    std::vector<Change> piece;
    std::string arena;
    bool halted = false;
    u64 last_t = 0;
    std::vector<u64> val, xz;

    // The most libfst may allocate at once: no part of a file uncompresses
    // to more than about a thousand times its size (zlib's most; LZ4's is
    // 255), and its time table is 8 bytes a time, each at least a byte.
    size_t most() const { return static_cast<size_t>(std::min<u64>(size, u64{1} << 40) * 16384 + (u64{64} << 20)); }

    void note(std::string s) {
        if (w->notes.size() < kMaxNotes) w->notes.push_back(std::move(s));
    }

    // Stops reading, keeping what was read: libfst goes on through the
    // file's remaining blocks, but with no variable asked for it only
    // reads their times (a millisecond or two a block).
    void halt(std::string why) {
        if (halted) return;
        halted = true;
        fstReaderClrFacProcessMaskAll(ctx);
        std::unique_lock lock(w->mu);
        if (w->error.empty()) w->error = std::move(why);
    }

    u64 bytes_at(u64 t) const {
        if (blocks.sections.empty()) {
            const u64 span = w->end > w->start ? w->end - w->start : 1;
            return static_cast<u64>(static_cast<unsigned __int128>(size) * (std::min(t, w->end) - std::min(t, w->start)) / span);
        }
        const auto it = std::ranges::lower_bound(blocks.sections, t, {}, [](const auto& s) { return s.first; });
        return it == blocks.sections.end() ? size : it->second;
    }

    void add(const Change& c) {
        Lane& l = w->lanes[c.lane];
        switch (l.kind()) {
            case LaneKind::kBits:
                if (l.width() <= 64) {
                    mem += l.add_bits(c.t, &c.a, c.b ? &c.b : nullptr);
                } else {
                    // ("1x0z", the first character the highest bit; a shorter
                    // one is widened with 0, a longer one keeps its lowest bits.)
                    const std::string_view s(arena.data() + c.a, c.n);
                    const u32 width = l.width(), words = (width + 63) / 64;
                    std::fill(val.begin(), val.begin() + words, 0);
                    std::fill(xz.begin(), xz.begin() + words, 0);
                    bool any = false;
                    for (u32 i = 0; i < std::min<size_t>(s.size(), width); ++i) {
                        const u8 d = kDigit.v[static_cast<u8>(s[s.size() - 1 - i])];
                        val[i / 64] |= static_cast<u64>(d & 1) << (i % 64);
                        xz[i / 64] |= static_cast<u64>(d >> 1) << (i % 64);
                        any |= d > 1;
                    }
                    mem += l.add_bits(c.t, val.data(), any ? xz.data() : nullptr);
                }
                break;
            case LaneKind::kReal: mem += l.add_real(c.t, std::bit_cast<double>(c.a)); break;
            case LaneKind::kText: mem += l.add_text(c.t, std::string_view(arena.data() + c.a, c.n)); break;
            case LaneKind::kEvent: mem += l.add_event(c.t); break;
        }
    }

    // The piece's changes into the lanes, under the Wave's lock; then the
    // budget, a stop asked for, and progress.
    void flush() {
        if (piece.empty()) return;
        {
            std::unique_lock lock(w->mu);
            for (const Change& c : piece) add(c);
            last_t = piece.back().t;
            w->reached = last_t;
            w->end = std::max(w->end, last_t);
        }
        piece.clear();
        arena.clear();
        if (max && mem > max) halt("the dump needs more memory than timeline.max_mb allows: read up to " + w->time_text(last_t));
        if (stop.stop_requested()) halt("stopped");
        if (*progress) (*progress)(bytes_at(last_t));
    }

    void change(u64 t, fstHandle h, const u8* v, u32 len) {
        if (halted) return;
        if (h == 0 || h > lane_of.size() || lane_of[h - 1] == ~0u) return;
        Change c{t, lane_of[h - 1], 0, 0, 0};
        const Lane& l = w->lanes[c.lane];
        switch (l.kind()) {
            case LaneKind::kBits:
                if (l.width() <= 64) {
                    // (The common case, read straight into the planes: a
                    // value's characters, as many as its declared bits, or
                    // fewer if a damaged file has them so.)
                    const u32 n = len ? len : static_cast<u32>(::strnlen(reinterpret_cast<const char*>(v), length_of[h - 1]));
                    u32 i = n > l.width() ? n - l.width() : 0;
                    // (Eight characters at a time while they are all 0 and
                    // 1: their low bits gathered into a byte, the first the
                    // highest, by one multiply.)
                    if constexpr (std::endian::native == std::endian::little)
                        for (u64 eight; i + 8 <= n; i += 8) {
                            std::memcpy(&eight, v + i, 8);
                            if ((eight & 0xFEFEFEFEFEFEFEFEull) != 0x3030303030303030ull) break;
                            c.a = c.a << 8 | ((eight & 0x0101010101010101ull) * 0x8040201008040201ull) >> 56;
                            c.b <<= 8;
                        }
                    for (; i < n; ++i) {
                        const u8 d = kDigit.v[v[i]];
                        c.a = c.a << 1 | (d & 1);
                        c.b = c.b << 1 | (d >> 1);
                    }
                    break;
                }
                [[fallthrough]];
            case LaneKind::kText: {
                const char* s = reinterpret_cast<const char*>(v);
                const u32 n = len ? len : static_cast<u32>(l.kind() == LaneKind::kText ? std::strlen(s) : ::strnlen(s, length_of[h - 1]));
                c.a = arena.size();
                c.n = n;
                arena.append(s, n);
                break;
            }
            case LaneKind::kReal: std::memcpy(&c.a, v, 8); break;
            case LaneKind::kEvent: break;
        }
        piece.push_back(c);
        if (arena.size() + piece.size() * sizeof(Change) >= kPiece) flush();
    }

    static void on_change(void* self, uint64_t t, fstHandle h, const unsigned char* v) { static_cast<Fst*>(self)->change(t, h, v, 0); }
    static void on_change_len(void* self, uint64_t t, fstHandle h, const unsigned char* v, uint32_t len) {
        static_cast<Fst*>(self)->change(t, h, v, len);
    }

    // The scopes and variables, and a lane for each handle.
    void hierarchy() {
        Wave& wv = *w;
        u32 scope = 0;
        for (;;) {
            fstHier* h = nullptr;
            if (!guarded(most(), [&] { h = fstReaderIterateHier(ctx); })) {
                wv.error = "the FST file is damaged: libfst could not read its hierarchy";
                return;
            }
            if (!h) break;
            if (h->htyp == FST_HT_SCOPE) {
                WaveScope s;
                s.kind = h->u.scope.typ <= FST_ST_VCD_PROGRAM ? static_cast<ScopeKind>(h->u.scope.typ) : ScopeKind::kOther;
                s.name.assign(h->u.scope.name, h->u.scope.name_length);
                s.parent = scope;
                const u32 id = static_cast<u32>(wv.scopes.size());
                mem += sizeof s + s.name.size();
                wv.scopes.push_back(std::move(s));
                wv.scopes[scope].scopes.push_back(id);
                scope = id;
            } else if (h->htyp == FST_HT_UPSCOPE) {
                if (scope == 0) note("an upscope with no scope open");
                scope = wv.scopes[scope].parent;
            } else if (h->htyp == FST_HT_VAR) {
                var(h->u.var, scope);
                if (max && mem > max) {
                    wv.error = "the dump's definitions take more memory than timeline.max_mb allows";
                    return;
                }
            }
            // (Attributes, FST_HT_ATTRBEGIN and FST_HT_ATTREND: source
            // files and lines, enums' tables, arrays: not kept yet.)
        }
    }

    void var(const decltype(fstHier::u.var)& f, u32 scope) {
        Wave& wv = *w;
        WaveVar v;
        v.kind = f.typ <= FST_VT_MAX ? kVarKinds[f.typ] : VarKind::kOther;
        v.name.assign(f.name, f.name_length);
        v.code = std::to_string(f.handle);
        v.scope = scope;
        // "count [7:0]", "data [3]": the declared range, after a space (as
        // Verilator and Icarus write it); "count[7:0]" with no space too, but
        // not "mem[3]", an element of an array.
        if (v.name.ends_with(']')) {
            const size_t open = v.name.rfind('[');
            if (open != std::string::npos && open > 0 && (v.name[open - 1] == ' ' || v.name.find(':', open) != std::string::npos) &&
                parse_range(std::string_view(v.name).substr(open), &v.msb, &v.lsb)) {
                v.ranged = true;
                v.name.resize(open);
                while (v.name.ends_with(' ')) v.name.pop_back();
            }
        }
        const bool real = f.typ == FST_VT_VCD_REAL || f.typ == FST_VT_VCD_REAL_PARAMETER || f.typ == FST_VT_VCD_REALTIME || f.typ == FST_VT_SV_SHORTREAL;
        const LaneKind lk = real ? LaneKind::kReal : f.typ == FST_VT_GEN_STRING ? LaneKind::kText : f.typ == FST_VT_VCD_EVENT ? LaneKind::kEvent : LaneKind::kBits;
        // (A real is declared 8 bytes long, or 4: as VCD has it, 64 bits.)
        v.width = real ? 64 : f.length;
        if (lk == LaneKind::kBits && v.width == 0) {
            note("a variable of no bits, taken as one: " + v.name);
            v.width = 1;
        }
        if (v.width > kMaxWidth) {
            note("a variable wider than " + std::to_string(kMaxWidth) + " bits, taken as that wide: " + v.name);
            v.width = kMaxWidth;
        }
        if (f.handle == 0) {
            note("a variable with no handle: " + v.name);
            return;
        }
        if (f.handle > lane_of.size()) {
            lane_of.resize(f.handle, ~0u);
            length_of.resize(f.handle, 0);
        }
        u32& lane = lane_of[f.handle - 1];
        if (lane != ~0u) {
            const Lane& l = wv.lanes[lane];
            if (l.kind() != lk || (lk == LaneKind::kBits && l.width() != v.width))
                note("an alias of another kind or width than the first with its handle: " + v.name);
        } else {
            lane = static_cast<u32>(wv.lanes.size());
            length_of[f.handle - 1] = f.length;
            wv.lanes.emplace_back(lk, v.width);
            mem += sizeof(Lane);
            if (lk == LaneKind::kBits && val.size() < (v.width + 63) / 64) {
                val.resize((v.width + 63) / 64);
                xz.resize(val.size());
            }
        }
        v.lane = lane;
        mem += sizeof v + v.name.size() + v.code.size() + 8;
        wv.scopes[scope].vars.push_back(static_cast<u32>(wv.vars.size()));
        wv.vars.push_back(std::move(v));
    }

    // The header: the timescale (a power of ten, as 1, 10 or 100 of s, ms,
    // us, ns, ps or fs), the span, $dumpoff stretches, date and version.
    void header() {
        Wave& wv = *w;
        i32 e = fstReaderGetTimescale(ctx);
        u32 mult = 1;
        while (e % 3 != 0) mult *= 10, --e;
        wv.unit_mult = mult;
        wv.unit_exp = e;
        wv.unit_given = true;
        wv.start = fstReaderGetStartTime(ctx);
        wv.end = fstReaderGetEndTime(ctx);
        if (const i64 zero = fstReaderGetTimezero(ctx)) note("a time zero of " + std::to_string(zero) + ", not added to the times");
        auto text = [](const char* s) {
            std::string t = s ? s : "";
            while (!t.empty() && (t.back() == '\n' || t.back() == ' ' || t.back() == '\t')) t.pop_back();
            while (!t.empty() && (t.front() == ' ' || t.front() == '\t')) t.erase(0, 1);
            return t;
        };
        wv.date = text(fstReaderGetDateString(ctx));
        wv.version = text(fstReaderGetVersionString(ctx));
        const u32 n = fstReaderGetNumberDumpActivityChanges(ctx);
        for (u32 i = 0; i < n; ++i) {
            const u64 t = fstReaderGetDumpActivityChangeTime(ctx, i);
            const bool on = fstReaderGetDumpActivityChangeValue(ctx, i) != 0;
            if (!on && (wv.off.empty() || wv.off.back().second != ~0ull)) wv.off.push_back({t, ~0ull});
            else if (on && !wv.off.empty() && wv.off.back().second == ~0ull) wv.off.back().second = t;
        }
    }

    void read(const std::string& path) {
        Wave& wv = *w;
        const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        struct stat st{};
        if (fd < 0 || ::fstat(fd, &st) != 0) {
            const int e = errno;
            if (fd >= 0) ::close(fd);
            std::unique_lock lock(wv.mu);
            wv.error = std::strerror(e);
            return;
        }
        size = static_cast<u64>(st.st_size);
        blocks = walk(fd, size);
        ::close(fd);
        if (blocks.wrong.empty() && !blocks.hier) blocks.wrong = "the FST file has no hierarchy: its writer has not finished it";
        if (!blocks.wrong.empty()) {
            std::unique_lock lock(wv.mu);
            wv.error = blocks.wrong;
            return;
        }
        if (!guarded(most(), [&] { ctx = fstReaderOpen(path.c_str()); }) || !ctx) {
            ctx = nullptr;
            std::unique_lock lock(wv.mu);
            wv.error = "the FST file is damaged: libfst could not read its header";
            return;
        }
        {
            std::unique_lock lock(wv.mu);
            header();
            hierarchy();
            wv.header = true;
        }
        if (!wv.error.empty() || stop.stop_requested()) return;
        if (*progress) (*progress)(0);
        fstReaderSetFacProcessMaskAll(ctx);
        fstReaderIterBlocksSetNativeDoublesOnCallback(ctx, 1);
        piece.reserve(kPiece / sizeof(Change));
        if (!guarded(most(), [&] { fstReaderIterBlocks2(ctx, on_change, on_change_len, this, nullptr); })) {
            // (Its state is not to be trusted, nor closed: kept, with the
            // memory it holds.)
            ctx = nullptr;
            piece.clear();
            std::unique_lock lock(wv.mu);
            if (wv.error.empty()) wv.error = "the FST file is damaged: libfst stopped reading it after " + wv.time_text(last_t);
            return;
        }
        flush();
    }
};

}  // namespace

WaveFormat wave_format(std::string_view head) {
    if (head.size() < 9) return WaveFormat::kVcd;
    const u8 type = static_cast<u8>(head[0]);
    const u64 len = be64(reinterpret_cast<const u8*>(head.data()) + 1);
    return (type == kHdr && len == kHdrLength) || (type == kZWrapper && len > 0) ? WaveFormat::kFst : WaveFormat::kVcd;
}

u64 read_fst(Wave* into, const std::string& path, u64 max_bytes, std::stop_token stop, const std::function<void(u64)>& progress) {
    Fst f;
    f.w = into, f.max = max_bytes, f.stop = stop, f.progress = &progress;
    f.read(path);
    if (f.ctx) fstReaderClose(f.ctx);
    std::unique_lock lock(into->mu);
    if (stop.stop_requested() && into->error.empty()) into->error = "stopped";
    u64 changes = 0;
    for (Lane& l : into->lanes) {
        l.shrink();
        changes += l.size();
    }
    into->changes = changes;
    into->reached = into->end;
    into->header = into->done = true;
    return f.mem;
}

WaveLoad::WaveLoad(std::shared_ptr<Wave> into, std::string fst_path, u64 max_bytes, std::function<void()> notify)
    : wave_(std::move(into)), notify_(std::move(notify)) {
    struct stat st{};
    if (::stat(fst_path.c_str(), &st) == 0) size_ = static_cast<u64>(st.st_size);
    thread_ = std::jthread([this, path = std::move(fst_path), max_bytes](std::stop_token stop) { run_fst(stop, path, max_bytes); });
}

void WaveLoad::run_fst(std::stop_token stop, const std::string& path, u64 max_bytes) {
    thread_soon(Soon::kWaitedFor);
    u64 told = 0;
    bool header_told = false;
    read_fst(wave_.get(), path, max_bytes, stop, [&](u64 at) {
        read_.store(at, std::memory_order_relaxed);
        if (!notify_) return;
        const u64 now = now_ns();
        if (!header_told || now - told >= 100'000'000) {
            header_told = true;
            told = now;
            notify_();
        }
    });
    read_.store(size_, std::memory_order_relaxed);
    finished_.store(true, std::memory_order_release);
    if (notify_) notify_();
}

}  // namespace blit
