#include <cstring>
#include "editor.hpp"

#include "remote.hpp"

#include <algorithm>

#include "scan.hpp"
#include "utf8.hpp"

namespace blit {

namespace {

// 0 = whitespace, 1 = word (letters, digits, '_', any non-ASCII), 2 = other
int char_class(u8 c) {
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') return 0;
    if (c >= 0x80 || c == '_' || (c >= '0' && c <= '9') || ((c | 0x20) >= 'a' && (c | 0x20) <= 'z'))
        return 1;
    return 2;
}

// True when the CR that ends the reader's chunk is followed by a newline, or
// by nothing: the next piece is looked at, so a CRLF cut in two still counts.
bool cr_ends_line(const Buffer::Reader& rd, size_t n) {
    Buffer::Reader next = rd;
    next.advance(static_cast<u32>(n));
    const std::string_view c = next.chunk();
    return c.empty() || c[0] == '\n';
}

bool is_control(u32 cp) { return cp < 0x20 || cp == 0x7F; }

// The code point that ends at `off` (> 0): where it starts, and what it is
// (U+FFFD for a broken one, a byte long; a CRLF is one, a newline).
u64 point_before(const Buffer& buf, u64 off, u32* cp) {
    char b[4];
    const u32 k = static_cast<u32>(std::min<u64>(off, 4));
    buf.read(off - k, b, k);
    *cp = 0xFFFD;
    if (k >= 2 && b[k - 1] == '\n' && b[k - 2] == '\r') {
        *cp = '\n';
        return off - 2;
    }
    for (u32 back = 1; back <= k; ++back) {
        if (is_cont(static_cast<u8>(b[k - back]))) continue;
        const Decoded d = decode_utf8(reinterpret_cast<const u8*>(&b[k - back]), back);
        if (d.len != back) return off - 1;
        *cp = d.cp;
        return off - back;
    }
    return off - 1;
}

}  // namespace

// ---------------------------------------------------------------- files

bool Editor::open(const char* file, std::string* err) {
    if (is_remote(file)) {  // (another machine's is read on a thread, and given to open_bytes)
        *err = std::string("cannot open here: ") + file;
        return false;
    }
    if (!buf.load_file(file, err)) return false;
    opened(file);
    return true;
}

void Editor::open_bytes(const std::string& file, std::string_view bytes) {
    buf.load_bytes(bytes);
    opened(file);
}

void Editor::opened(const std::string& file) {
    path = file;
    cursor_ = anchor_ = 0;
    want_col_ = ~0u;
    undo_.clear();
    redo_.clear();
    saved_id_ = 0;
    ++revision_;
    edits_.note(revision_, 0);  // (all of it)
    // The first line break decides what Enter inserts.
    char head[4096];
    u64 n = buf.read(0, head, sizeof head);
    const char* nl = static_cast<const char*>(std::memchr(head, '\n', n));
    crlf_ = nl && nl > head && nl[-1] == '\r';
}

bool Editor::save(std::string* err, i64* modified_ns) {
    if (path.empty()) {
        if (err) *err = "no file name";
        return false;
    }
#if !defined(_WIN32)
    if (is_remote(path)) {  // to another machine: the text whole, through its agent
        std::string there;
        const std::shared_ptr<Remote> r = remote_for(path, &there);  // (no waiting: the connection is up, or saving fails)
        if (!r) return *err = remote_error(path), false;
        if (!r->write(there, buf.text(0, buf.size()), err, modified_ns)) return false;
    } else
#endif
        if (!buf.save_file(path.c_str(), err)) return false;
    if (!undo_.empty()) undo_.back().sealed = true;
    saved_id_ = state_id();
    return true;
}

// ---------------------------------------------------------------- geometry

u64 Editor::widest_columns(std::span<const Piece> pieces, u32 tab_width, std::stop_token stop) {
    const u64 tab = std::max<u32>(1, tab_width);
    u64 best = 0;
    // A line is walked for its exact width only if it could be the widest:
    // one of n bytes is at most n * tab columns wide. Most lines are ruled
    // out by their length alone, which a search for the newline gives.
    // The bytes of the line under way, when it is split between pieces:
    std::vector<std::string_view> parts;
    u64 part_bytes = 0;
    auto measure = [&](std::string_view last) {
        const u64 bytes = part_bytes + last.size();
        if (bytes * tab > best) {
            u64 col = 0;
            Joiner join;
            auto walk = [&](const std::string_view& sv) {
                const u8* p = reinterpret_cast<const u8*>(sv.data());
                const size_t n = sv.size();
                size_t i = 0;
                while (i < n) {
                    // (Long runs are taken a slice at a time so that a
                    // request to stop is heard inside a 64 MB line too.)
                    const size_t run = ascii_run(p + i, std::min<size_t>(n - i, 1u << 16));
                    col += run;
                    i += run;
                    if (run) join.reset();
                    if (i >= n) break;
                    if (p[i] >= 0x20 && p[i] < 0x7F) {
                        if (stop.stop_requested()) return;
                        continue;  // the slice ended, not the run
                    }
                    if (p[i] == '\t') {
                        col = (col / tab + 1) * tab;
                        ++i;
                        join.reset();
                    } else if (p[i] == '\r' && i + 1 == n && &sv == &last) {
                        ++i;  // the CR of a CRLF takes no space
                    } else if (p[i] < 0x80) {
                        ++col;
                        ++i;
                        join.reset();
                    } else {
                        const Decoded d = decode_utf8(p + i, n - i);
                        col += join.width(d.cp);
                        i += d.len;
                    }
                }
            };
            for (const std::string_view& sv : parts) walk(sv);
            walk(last);
            best = std::max(best, col);
        }
        parts.clear();
        part_bytes = 0;
    };
    for (const Piece& pc : pieces) {
        if (stop.stop_requested()) return best;
        const char* p = pc.p;
        size_t n = pc.len;
        while (n > 0) {
            const char* nl = static_cast<const char*>(std::memchr(p, '\n', n));
            if (!nl) {
                parts.emplace_back(p, n);
                part_bytes += n;
                break;
            }
            measure(std::string_view(p, static_cast<size_t>(nl - p)));
            n -= static_cast<size_t>(nl - p) + 1;
            p = nl + 1;
        }
    }
    measure({});
    return best;
}

u32 Editor::guess_indent(bool* tabs) const {
    constexpr u64 kLines = 4000;
    constexpr u64 kBytes = 256 << 10;  // nor further than this, whatever the lines are like
    constexpr u32 kMaxStep = 8;
    u32 votes[kMaxStep + 1] = {};
    u32 tabbed = 0, spaced = 0;
    Buffer::Reader r = buf.reader(0);
    u32 prev = 0;       // leading spaces of the last line that had text
    u32 lead = 0;       // of this one, so far
    bool in_lead = true, tab_led = false;
    u64 lines = 0, bytes = 0;
    auto end_lead = [&] {  // the line's first character that is not indentation
        in_lead = false;
        if (tab_led) {
            ++tabbed;
        } else {
            if (lead > 0) ++spaced;
            if (lead > prev && lead - prev <= kMaxStep) ++votes[lead - prev];
            prev = lead;
        }
    };
    while (lines < kLines && bytes < kBytes) {
        std::string_view c = r.chunk();
        if (c.empty()) break;
        c = c.substr(0, static_cast<size_t>(std::min<u64>(c.size(), kBytes - bytes)));
        bytes += c.size();
        size_t i = 0;
        while (i < c.size() && lines < kLines) {
            const char b = c[i];
            if (b == '\n') {
                ++lines;
                lead = 0;
                in_lead = true;
                tab_led = false;
                ++i;
            } else if (!in_lead) {
                // The rest of the line does not matter: on to its end.
                const void* nl = std::memchr(c.data() + i, '\n', c.size() - i);
                i = nl ? static_cast<size_t>(static_cast<const char*>(nl) - c.data()) : c.size();
            } else if (b == ' ') {
                ++lead;
                ++i;
            } else if (b == '\t') {
                tab_led = true;
                ++i;
            } else if (b == '\r') {
                ++i;
            } else {
                end_lead();
            }
        }
        r.advance(static_cast<u32>(c.size()));
    }
    if (tabs) *tabs = tabbed > spaced || (tabbed == 0 && spaced == 0);
    if (tabbed > spaced) return std::max<u32>(1, tab_width);
    u32 step = 0;
    for (u32 k = 2; k <= kMaxStep; ++k)
        if (votes[k] > votes[step]) step = k;
    return step ? step : std::max<u32>(1, tab_width);
}

u64 Editor::line_end(u64 line) const {
    if (line + 1 >= buf.line_count()) return buf.size();
    u64 e = buf.line_start(line + 1) - 1;
    char c;
    if (e > 0 && buf.read(e - 1, &c, 1) == 1 && c == '\r') --e;
    return e;
}

// A character here is a grapheme cluster: a letter with the marks on it,
// an emoji sequence with its joiners, a flag (see cp_extends() and Joiner).
// After a control character (a tab, a line break) one always ends.
u64 Editor::next_char(u64 off) const {
    const u64 size = buf.size();
    if (off >= size) return size;
    char b[16];
    u64 n = buf.read(off, b, sizeof b);
    const u8* p = reinterpret_cast<const u8*>(b);
    if (p[0] == '\r' && n >= 2 && p[1] == '\n') return off + 2;
    if (is_control(p[0])) return off + 1;
    Decoded d = decode_utf8(p, n);
    Joiner join;
    join.joins(d.cp);
    u64 i = d.len;
    for (;;) {
        if (i + 4 > n && off + n < size) {  // (a whole code point in the window)
            off += i;
            n = buf.read(off, b, sizeof b);
            i = 0;
        }
        if (i >= n || p[i] < 0x80) return off + i;  // (nothing in ASCII goes on with a cluster)
        d = decode_utf8(p + i, n - i);
        if (!join.extends(d.cp)) return off + i;
        i += d.len;
    }
}

u64 Editor::prev_char(u64 off) const {
    if (off == 0) return 0;
    u32 cp = 0;
    u64 at = point_before(buf, off, &cp);
    // While the character at `at` goes on with the one before it, the cluster starts further back.
    while (at > 0 && cp >= 0xA9) {
        u32 before = 0;
        const u64 from = point_before(buf, at, &before);
        if (is_control(before)) break;
        if (cp_regional(cp) && cp_regional(before)) {
            // Indicators pair up from the first of a run: this one is the second of a pair if an odd number come before it.
            u32 run = 1;
            for (u64 q = from; q > 0 && run < 64;) {
                u32 c = 0;
                q = point_before(buf, q, &c);
                if (!cp_regional(c)) break;
                ++run;
            }
            if (run % 2) at = from;
            break;
        }
        if (cp >= 0x600 && joins_back(from, before, cp)) {
            at = from;
            cp = before;
            continue;
        }
        if (!cp_extends(cp) && !(before == 0x200D && cp_pictographic(cp))) break;
        at = from;
        cp = before;
    }
    return at;
}

// The rules of Joiner that look back, from cp (after the character
// `before`, which starts at `from`): a prepended sign before it, Hangul's
// jamo making a syllable, or a consonant that a linker (with zero-width
// marks around it) joins to the consonant before.
bool Editor::joins_back(u64 from, u32 before, u32 cp) const {
    if (cp_prepend(before)) return true;
    if (hangul_joins(hangul_kind(before), hangul_kind(cp))) return true;
    if (!cp_conjunct_block(cp) || !ucd::in_ranges(ucd::kConsonant, cp)) return false;
    bool linker = false;
    u64 q = from;
    for (u32 c = before, steps = 0; steps < 32; ++steps) {
        if (ucd::in_ranges(ucd::kLinker, c)) linker = true;
        else if (!(cp_extends(c) && cp_width(c) == 0)) return linker && ucd::in_ranges(ucd::kConsonant, c);
        if (q == 0) return false;
        q = point_before(buf, q, &c);
    }
    return false;
}

u32 Editor::col_between(u64 line_start, u64 off) const {
    constexpr u64 kLong = 1u << 16;  // (shorter walks are not worth remembering)
    u64 from = line_start;
    u32 col = 0;
    // A remembered place holds while every edit since lies at or after it:
    // the text before it is as it was, and the ASCII character just before
    // it ends any cluster.
    auto holds = [&](const ColMemo& m) {
        return m.line_start == line_start && m.off <= off && m.tab_width == tab_width &&
               (m.revision == revision_ || edits_.changed_since(m.revision, revision_) >= m.off);
    };
    if (off - line_start >= kLong) {
        if (holds(col_near_)) {
            from = col_near_.off;
            col = col_near_.col;
        } else if (holds(col_far_)) {
            from = col_far_.off;
            col = col_far_.col;
        }
    }
    Buffer::Reader r = buf.reader(from);
    u64 left = off - from;
    Joiner join;
    // A place the walk passed at least 4 KB before `off`, between two ASCII
    // characters at the end of a piece, to be the older place remembered.
    const bool long_walk = off - line_start >= kLong;
    u64 pos = from, edge = 0;
    u32 edge_col = 0;
    ColMemo passed;
    while (left > 0) {
        std::string_view c = r.chunk();
        if (c.empty()) break;
        const u8* p = reinterpret_cast<const u8*>(c.data());
        if (edge && static_cast<u8>(c[0]) < 0x80) passed = {line_start, edge, revision_, edge_col, tab_width};
        edge = 0;
        size_t n = static_cast<size_t>(std::min<u64>(c.size(), left));
        size_t i = 0;
        while (i < n) {
            size_t run = ascii_run(p + i, n - i);
            col += static_cast<u32>(run);
            i += run;
            if (run) join.reset();
            if (i >= n) break;
            if (p[i] == '\t') {
                col = tab_stop(col);
                ++i;
                join.reset();
            } else if (p[i] < 0x80) {
                ++col;
                ++i;
                join.reset();
            } else {
                Decoded d = decode_utf8(p + i, c.size() - i);
                col += join.width(d.cp);
                i += d.len;
            }
        }
        // A multi-byte character may step past `left`; never past the chunk.
        size_t used = std::min(i, c.size());
        left -= std::min<u64>(used, left);
        r.advance(static_cast<u32>(used));
        pos += used;
        if (long_walk && used == c.size() && pos + 4096 <= off && pos - line_start >= kLong && p[used - 1] < 0x80) {
            edge = pos;
            edge_col = col;
        }
    }
    if (passed.off && !(holds(col_far_) && col_far_.off >= passed.off)) col_far_ = passed;
    // Remembered where the walk ended between two ASCII characters, a
    // place no cluster runs across (so that going on from it is the same as
    // walking all the way).
    if (off - line_start >= kLong && off > 0 && off < buf.size()) {
        char around[2];
        if (buf.read(off - 1, around, 2) == 2 && static_cast<u8>(around[0]) < 0x80 && static_cast<u8>(around[1]) < 0x80) {
            if (holds(col_near_) && col_near_.off + 4096 <= off) col_far_ = col_near_;
            col_near_ = {line_start, off, revision_, col, tab_width};
        }
    }
    return col;
}

u32 Editor::col_of(u64 off) const { return col_between(buf.line_start(buf.line_of(off)), off); }

u64 Editor::off_at(u64 line, u32 want, bool nearest) const {
    u64 off;
    Buffer::Reader r = buf.line_reader(line, &off);
    u32 col = 0;
    // The character (cluster) under way: where it starts, and its column.
    // Once the next one starts, it is known whether `want` was in it.
    u64 start = off;
    u32 start_col = 0;
    auto in_last = [&] { return (nearest && (want - start_col) * 2 >= col - start_col) ? off : start; };
    Joiner join;
    for (;;) {
        std::string_view c = r.chunk();
        if (c.empty()) return want < col ? in_last() : off;
        const u8* p = reinterpret_cast<const u8*>(c.data());
        size_t i = 0;
        while (i < c.size()) {
            u8 b = p[i];
            u32 w, len = 1;
            bool goes_on = false;
            if (b == '\n') return want < col ? in_last() : off;
            if (b == '\r') {
                if (i + 1 < c.size() ? p[i + 1] == '\n' : cr_ends_line(r, c.size())) return want < col ? in_last() : off;
            }
            if (b == '\t') {
                w = tab_stop(col) - col;
                join.reset();
            } else if (b < 0x80) {
                w = 1;
                join.reset();
            } else {
                Decoded d = decode_utf8(p + i, c.size() - i);
                const u8 how = join.joins(d.cp);
                goes_on = how != kJoinNone || cp_extends(d.cp);
                w = how == kJoinZeroWidth ? 0 : cp_width(d.cp);
                len = d.len;
            }
            if (!goes_on) {
                if (want < col) return in_last();
                start = off;
                start_col = col;
            }
            col += w;
            off += len;
            i += len;
        }
        r.advance(static_cast<u32>(c.size()));
    }
}

// ---------------------------------------------------------------- movement

void Editor::move_to(u64 off, bool select) {
    cursor_ = std::min(off, buf.size());
    if (!select) anchor_ = cursor_;
    want_col_ = ~0u;
}

void Editor::left(bool select) {
    if (!select && has_selection()) return move_to(sel_min(), false);
    move_to(prev_char(cursor_), select);
}

void Editor::right(bool select) {
    if (!select && has_selection()) return move_to(sel_max(), false);
    move_to(next_char(cursor_), select);
}

void Editor::vertical(i64 lines, bool select) {
    u64 line = buf.line_of(cursor_);
    u32 col = want_col_ != ~0u ? want_col_ : col_between(buf.line_start(line), cursor_);
    u64 last = buf.line_count() - 1;
    if (skip) {  // (by rows: a fold is one)
        const u64 row = skip->skip_row(line), rows = skip->skip_rows();
        if (lines < 0 ? row == 0 : row + 1 >= rows) {
            move_to(lines < 0 ? 0 : buf.size(), select);
            return;
        }
        const u64 to_row = lines < 0 ? (static_cast<u64>(-lines) > row ? 0 : row - static_cast<u64>(-lines)) : std::min(rows - 1, row + static_cast<u64>(lines));
        move_to(off_at(std::min(last, skip->skip_line(to_row)), col), select);
        want_col_ = col;
        return;
    }
    u64 to;
    if (lines < 0) {
        u64 up = static_cast<u64>(-lines);
        if (line == 0) {
            move_to(0, select);
            return;
        }
        to = up > line ? 0 : line - up;
    } else {
        if (line == last) {
            move_to(buf.size(), select);
            return;
        }
        to = std::min(last, line + static_cast<u64>(lines));
    }
    move_to(off_at(to, col), select);
    want_col_ = col;
}

// Next word boundary from off in direction dir (-1 or +1): skips anything
// that is not a word character, then the word itself. It goes a character
// (a cluster) at a time, and a character is of the class of its first
// byte: a mark on a space or a sign is no part of a word. Bytes are read a
// window at a time, and ASCII is a byte a character with no more asked.
u64 Editor::word_edge(u64 off, int dir) const {
    const u64 size = buf.size();
    char win[256];
    u64 base = 0, have = 0;  // win holds the bytes [base, base + have)
    auto byte = [&](u64 at) {
        if (at < base || at >= base + have) {
            base = dir > 0 ? at : at + 1 - std::min<u64>(at + 1, sizeof win);
            have = buf.read(base, win, sizeof win);
        }
        return static_cast<u8>(win[at - base]);
    };
    bool in_word = false;
    while (dir < 0 ? off > 0 : off < size) {
        u8 c;
        u64 next;
        if (dir > 0) {
            c = byte(off);
            next = c < 0x80 && off + 1 < size && byte(off + 1) < 0x80 ? off + 1 : next_char(off);
        } else {
            c = byte(off - 1);
            next = c < 0x80 ? off - 1 : prev_char(off);
            if (c >= 0x80) c = byte(next);
        }
        const bool word = char_class(c) == 1;
        if (in_word && !word) return off;
        in_word = in_word || word;
        off = next;
    }
    return off;
}

// A word starts and ends where a character does. Inside a cluster: its
// end, unless that is not before `limit`; then its start.
u64 Editor::cluster_edge(u64 off, u64 limit) const {
    if (off == 0 || off >= buf.size()) return off;
    const u64 start = prev_char(off), end = next_char(start);
    if (end == off) return off;
    return end < limit ? end : start;
}

void Editor::word_left(bool select) { move_to(word_edge(cursor_, -1), select); }
void Editor::word_right(bool select) { move_to(word_edge(cursor_, 1), select); }

void Editor::home(bool select) { move_to(buf.line_start(buf.line_of(cursor_)), select); }
void Editor::end(bool select) { move_to(line_end(buf.line_of(cursor_)), select); }

void Editor::select_all() {
    anchor_ = 0;
    cursor_ = buf.size();
    want_col_ = ~0u;
}

void Editor::select_word(u64 off) {
    off = std::min(off, buf.size());
    char c;
    if (buf.read(off, &c, 1) != 1 || c == '\n') {
        move_to(off, false);
        return;
    }
    int cls = char_class(static_cast<u8>(c));
    u64 lo = off, hi = off + 1;
    char win[256];
    for (bool going = true; going && lo > 0;) {
        const u64 n = buf.read(lo - std::min<u64>(lo, sizeof win), win, std::min<u64>(lo, sizeof win));
        going = false;
        for (u64 k = n; k > 0; --k) {
            if (win[k - 1] == '\n' || char_class(static_cast<u8>(win[k - 1])) != cls) break;
            --lo;
            going = k == 1;
        }
    }
    for (bool going = true; going;) {
        const u64 n = buf.read(hi, win, sizeof win);
        going = false;
        for (u64 k = 0; k < n; ++k) {
            if (win[k] == '\n' || char_class(static_cast<u8>(win[k])) != cls) break;
            ++hi;
            going = k + 1 == n;
        }
    }
    anchor_ = cluster_edge(lo, off + 1);
    cursor_ = cluster_edge(hi, ~0ull);
    want_col_ = ~0u;
}

void Editor::select_line(u64 off) {
    u64 line = buf.line_of(std::min(off, buf.size()));
    anchor_ = buf.line_start(line);
    cursor_ = buf.line_start(line + 1);
    want_col_ = ~0u;
}

// ---------------------------------------------------------------- edits

// The listener needs positions from before and after the change, so an edit
// is bracketed by these two. They do nothing when nobody listens.
TextEdit Editor::begin_edit(u64 off, u64 remove_len) const {
    TextEdit t;
    if (!listening()) return t;
    for (EditListener* l : listeners_)
        if (l) l->before_edit(off, remove_len);
    t.start = off;
    t.old_end = off + remove_len;
    const u64 row = buf.line_of(off), row_start = buf.line_start(row);
    t.start_row = static_cast<u32>(row);
    t.start_col = static_cast<u32>(off - row_start);
    const u64 end_row = remove_len ? buf.line_of(t.old_end) : row;
    t.old_end_row = static_cast<u32>(end_row);
    t.old_end_col = static_cast<u32>(t.old_end - (end_row == row ? row_start : buf.line_start(end_row)));
    return t;
}

void Editor::end_edit(TextEdit& t, u64 insert_len) {
    if (!listening()) return;
    t.new_end = t.start + insert_len;
    const u64 end_row = buf.line_of(t.new_end);
    t.new_end_row = static_cast<u32>(end_row);
    t.new_end_col = static_cast<u32>(t.new_end - buf.line_start(end_row));
    for (EditListener* l : listeners_)
        if (l) l->on_edit(t);
}

void Editor::apply(u64 off, u64 remove_len, std::string_view text, Kind kind) {
    if (remove_len == 0 && text.empty()) return;
    TextEdit note = begin_edit(off, remove_len);
    Edit e;
    e.off = off;
    e.cur_len = text.size();
    e.cursor0 = cursor_;
    e.anchor0 = anchor_;
    e.kind = kind;
    if (remove_len) buf.erase(off, remove_len, &e.stash);
    if (!text.empty()) buf.insert(off, text);
    end_edit(note, text.size());
    if (watch) watch->edited(off, off + remove_len, off + text.size());
    if (views) views->edited(off, off + remove_len, off + text.size());
    cursor_ = anchor_ = e.cursor1 = off + text.size();
    want_col_ = ~0u;
    ++revision_;
    edits_.note(revision_, off);
    redo_.clear();

    // Runs of typing, backspace and forward delete fold into one undo step.
    if (!undo_.empty() && !undo_.back().sealed && undo_.back().kind == kind) {
        Edit& p = undo_.back();
        if (kind == kTyping && remove_len == 0 && p.off + p.cur_len == off && text != "\n" && text != "\r\n") {
            p.cur_len += text.size();
            p.cursor1 = e.cursor1;
            return;
        }
        if (kind == kBackspace && p.cur_len == 0 && off + remove_len == p.off) {
            // Each backspace removes text just before the previous one, so
            // the stash grows at the front. Keeping it reversed makes that an
            // append, and neighbours in memory fold into one piece.
            if (!p.reversed) {
                std::reverse(p.stash.begin(), p.stash.end());
                p.reversed = true;
            }
            for (size_t i = e.stash.size(); i-- > 0;) {
                const Piece& n = e.stash[i];
                if (!p.stash.empty() && n.p + n.len == p.stash.back().p &&
                    n.len + p.stash.back().len <= Buffer::kMaxPiece) {
                    Piece& last = p.stash.back();
                    last.p = n.p;
                    last.len += n.len;
                    last.nl += n.nl;
                } else {
                    p.stash.push_back(n);
                }
            }
            p.off = off;
            p.cursor1 = e.cursor1;
            return;
        }
        if (kind == kDelete && p.cur_len == 0 && off == p.off) {
            for (const Piece& n : e.stash) {
                if (!p.stash.empty() && p.stash.back().p + p.stash.back().len == n.p &&
                    n.len + p.stash.back().len <= Buffer::kMaxPiece) {
                    p.stash.back().len += n.len;
                    p.stash.back().nl += n.nl;
                } else {
                    p.stash.push_back(n);
                }
            }
            return;
        }
    }
    e.id = next_id_++;
    undo_.push_back(std::move(e));
}

void Editor::swap_edit(Edit& e) {
    if (e.reversed) {
        std::reverse(e.stash.begin(), e.stash.end());
        e.reversed = false;
    }
    TextEdit note = begin_edit(e.off, e.cur_len);
    std::vector<Piece> out;
    if (e.cur_len) buf.erase(e.off, e.cur_len, &out);
    u64 back = 0;
    for (const Piece& p : e.stash) back += p.len;
    buf.insert_pieces(e.off, e.stash);
    end_edit(note, back);
    if (watch) watch->edited(e.off, e.off + e.cur_len, e.off + back);
    if (views) views->edited(e.off, e.off + e.cur_len, e.off + back);
    e.stash = std::move(out);
    e.cur_len = back;
    e.sealed = true;
    ++revision_;
    edits_.note(revision_, e.off);
}

bool Editor::undo() {
    if (undo_.empty()) return false;
    const u64 group = undo_.back().group;
    changed_at_ = ~0ull;
    do {  // the step, and with it the rest of its group
        Edit e = std::move(undo_.back());
        undo_.pop_back();
        swap_edit(e);
        cursor_ = e.cursor0;
        anchor_ = e.anchor0;
        changed_at_ = std::min(changed_at_, e.off);
        redo_.push_back(std::move(e));
    } while (group && !undo_.empty() && undo_.back().group == group);
    want_col_ = ~0u;
    return true;
}

bool Editor::redo() {
    if (redo_.empty()) return false;
    const u64 group = redo_.back().group;
    changed_at_ = ~0ull;
    do {
        Edit e = std::move(redo_.back());
        redo_.pop_back();
        swap_edit(e);
        cursor_ = anchor_ = e.cursor1;
        changed_at_ = std::min(changed_at_, e.off);
        undo_.push_back(std::move(e));
    } while (group && !redo_.empty() && redo_.back().group == group);
    want_col_ = ~0u;
    return true;
}

void Editor::insert(std::string_view text) {
    bool sel = has_selection();
    apply(sel_min(), sel_max() - sel_min(), text, sel ? kOther : kTyping);
}

void Editor::newline() { insert(crlf_ ? "\r\n" : "\n"); }

void Editor::backspace() {
    if (has_selection()) return apply(sel_min(), sel_max() - sel_min(), {}, kOther);
    u64 p = prev_char(cursor_);
    apply(p, cursor_ - p, {}, kBackspace);
}

void Editor::del() {
    if (has_selection()) return apply(sel_min(), sel_max() - sel_min(), {}, kOther);
    u64 n = next_char(cursor_);
    apply(cursor_, n - cursor_, {}, kDelete);
}

void Editor::backspace_word() {
    if (has_selection()) return backspace();
    u64 p = word_edge(cursor_, -1);
    apply(p, cursor_ - p, {}, kOther);
}

void Editor::delete_word() {
    if (has_selection()) return del();
    u64 n = word_edge(cursor_, 1);
    apply(cursor_, n - cursor_, {}, kOther);
}

}  // namespace blit
