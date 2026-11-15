#include "grid.hpp"

#include <algorithm>
#include <cstring>

#include "editor.hpp"
#include "scan.hpp"
#include "utf8.hpp"

namespace blit {

namespace {

constexpr u32 kNoEnd = ~0u;

// Records anchors while a row skips over the part of a long line that lies
// left of the window.
struct Recorder {
    AnchorCache* cache = nullptr;
    LineAnchors* la = nullptr;
    u64 line = 0;
    u32 next = LineAnchors::kStep;  // column where the next anchor is due

    void hit(u32 col, u64 off) {
        if (!la) la = &cache->claim(line);
        la->cols.push_back(col);
        la->offs.push_back(off);
        next = (col / LineAnchors::kStep + 1) * LineAnchors::kStep;
    }
};

// The slot of a character outside ASCII in italic or bold text. Rare enough
// to stay out of the row loop: it decodes the character a second time.
[[gnu::noinline, gnu::cold]] u32 styled_slot(Font& font, const u8* p, size_t n, u32 bits, u32 slot) {
    const Decoded d = decode_utf8(p, n);
    if (d.cp == 0xFFFD && d.len == 1) return slot;  // not a character: the box it already is
    return font.glyph(d.cp, (bits & Font::kStyleMask) >> Font::kStyleShift) & (Font::kWide - 1);
}

// Fills one grid row. The reader sits on the line at display column `col`
// and byte offset `off`, which is left at the start of the next line when
// the row reaches it. With a style cursor, each cell also gets its colour
// and its italic or bold (see StyleCursor: the bits to or into a cell).
// Returns the display column where the line ended, or kNoEnd if the row was
// full before the line was. `at_next` reports whether the reader was left at
// the start of the next line.
u32 fill_row(Buffer::Reader& rd, Font& font, u32 first_col, u32 cols, u32* row, bool* at_next, u32 col, u64& off,
             Recorder* rec, u32 tab_width, StyleCursor* sc) {
    const u32 end_col = first_col + cols;
    *at_next = false;
    for (;;) {
        std::string_view ch = rd.chunk();
        if (ch.empty()) {  // end of text
            *at_next = true;
            break;
        }
        const u8* p = reinterpret_cast<const u8*>(ch.data());
        const size_t n = ch.size();
        size_t i = 0;
        bool done = false;
        while (i < n) {
            if (col >= end_col) {
                rd.advance(static_cast<u32>(i));
                return kNoEnd;
            }
            const u8 b = p[i];
            if (static_cast<u8>(b - 0x20) < 0x5F) {
                // Printable ASCII: the byte is the glyph slot.
                if (col < first_col) {
                    size_t lim = std::min<size_t>(n - i, first_col - col);
                    if (rec && rec->next > col) lim = std::min<size_t>(lim, rec->next - col);
                    size_t run = ascii_run(p + i, lim);
                    col += static_cast<u32>(run);
                    i += run;
                    if (rec && col >= rec->next) rec->hit(col, off + i);
                } else {
                    size_t run = ascii_run(p + i, std::min<size_t>(n - i, end_col - col));
                    u32* dst = row + (col - first_col);
                    if (!sc) {
                        for (size_t k = 0; k < run; ++k) dst[k] = p[i + k];
                    } else {
                        // Same copy, one style run at a time.
                        const u64 pos = off + i;
                        for (size_t copied = 0; copied < run;) {
                            sc->advance(pos + copied);
                            const size_t seg = static_cast<size_t>(std::min<u64>(run - copied, sc->next - (pos + copied)));
                            const u32 bits = sc->bits;
                            const u8* src = p + i + copied;
                            u32* d = dst + copied;
                            for (size_t k = 0; k < seg; ++k) d[k] = src[k] | bits;
                            copied += seg;
                        }
                    }
                    col += static_cast<u32>(run);
                    i += run;
                }
                continue;
            }
            if (b == '\n') {
                ++i;
                *at_next = true;
                done = true;
                break;
            }
            u32 w = 1, len = 1, slot = Font::kTofu;
            if (b == '\t') {
                w = (col / tab_width + 1) * tab_width - col;
                slot = ' ';
            } else if (b == '\r' && (i + 1 >= n || p[i + 1] == '\n')) {
                ++i;  // the CR of a CRLF takes no space
                continue;
            } else if (b >= 0x80) {
                Decoded d = decode_utf8(p + i, n - i);
                len = d.len;
                if (d.cp != 0xFFFD || len != 1) {
                    const u32 g = font.glyph(d.cp);
                    slot = g & (Font::kWide - 1);
                    w = g & Font::kWide ? 2 : 1;
                }
            }
            u32 bits = 0;
            if (sc && col + w > first_col) {
                sc->advance(off + i);
                bits = sc->bits;
                // Outside ASCII the style is not part of the slot number:
                // the font is asked for the glyph in that style.
                if (bits & Font::kStyleMask) [[unlikely]] {
                    if (b >= 0x80) slot = styled_slot(font, p + i, n - i, bits, slot);
                    bits &= ~Font::kStyleMask;
                }
            }
            if (b == '\t') {
                for (u32 k = 0; k < w; ++k)
                    if (col + k >= first_col && col + k < end_col) row[col + k - first_col] = slot | bits;
            } else {
                for (u32 k = 0; k < w; ++k)
                    if (col + k >= first_col && col + k < end_col) row[col + k - first_col] = (slot + k) | bits;
            }
            col += w;
            i += len;
            if (rec && col >= rec->next && col <= first_col) rec->hit(col, off + i);
        }
        rd.advance(static_cast<u32>(i));
        off += i;
        if (done) break;
    }
    return col;
}

}  // namespace

void AnchorCache::sync(u64 doc, u64 revision) {
    if (revision == revision_ && doc == doc_) return;
    revision_ = revision;
    doc_ = doc;
    for (LineAnchors& l : lines_) l = LineAnchors{};
}

LineAnchors* AnchorCache::find(u64 line) {
    for (LineAnchors& l : lines_)
        if (l.line == line) {
            l.used = ++clock_;
            return &l;
        }
    return nullptr;
}

LineAnchors& AnchorCache::claim(u64 line) {
    if (LineAnchors* l = find(line)) return *l;
    LineAnchors* oldest = &lines_[0];
    for (LineAnchors& l : lines_)
        if (l.used < oldest->used) oldest = &l;
    *oldest = LineAnchors{};
    oldest->line = line;
    oldest->used = ++clock_;
    return *oldest;
}

void fill_grid(const Buffer& buf, Font& font, const GridSpec& g, u32* cells, AnchorCache* anchors) {
    const u64 lines = buf.line_count();
    const u32 lead = std::min(g.lead, g.cols);
    const u32 cols = g.cols - lead;  // columns that show text
    const u32 tab = std::max<u32>(1, g.tab_width);
    const bool far = anchors && g.first_col >= LineAnchors::kStep;
    if (anchors) anchors->sync(g.doc, g.revision);

    // With ligatures a row is filled a little wider than the window, so a
    // ligature that straddles the window edge is shaped whole, then the
    // visible part is copied out.
    const bool lig = font.ligatures();
    constexpr u32 kMargin = 16;
    const u32 back = lig ? std::min(g.first_col, kMargin) : 0;
    const u32 first_col = g.first_col - back;
    const u32 wide = cols + back + (lig ? kMargin : 0);
    static thread_local std::vector<u32> scratch;
    if (lig && scratch.size() < wide) scratch.resize(wide);

    StyleCursor style(g.highlights, g.style_bits);
    StyleCursor* sc = style.active() ? &style : nullptr;

    Buffer::Reader rd;
    bool positioned = false;
    u64 off = 0;  // byte offset of the reader
    for (u32 r = 0; r < g.rows; ++r) {
        u32* row = cells + static_cast<size_t>(r) * g.cols;
        const u64 line = g.top_line + r;
        if (line >= lines) {
            std::memset(row, 0, static_cast<size_t>(g.rows - r) * g.cols * sizeof(u32));
            break;
        }
        for (u32 k = 0; k < lead; ++k) row[k] = 0;
        row += lead;
        u32* dst = lig ? scratch.data() : row;

        u32 end;
        if (!far) {
            if (!positioned) rd = buf.line_reader(line, &off);
            end = fill_row(rd, font, first_col, wide, dst, &positioned, 0, off, nullptr, tab, sc);
        } else {
            // Scrolled far to the right: resume from the nearest anchor.
            Recorder rec{anchors, anchors->find(line), line};
            u32 col = 0;
            u64 anchor = 0;
            if (rec.la && !rec.la->cols.empty()) {
                const auto& ac = rec.la->cols;
                rec.next = (ac.back() / LineAnchors::kStep + 1) * LineAnchors::kStep;
                const auto it = std::upper_bound(ac.begin(), ac.end(), first_col);
                if (it != ac.begin()) {
                    const size_t k = static_cast<size_t>(it - ac.begin()) - 1;
                    col = ac[k];
                    anchor = rec.la->offs[k];
                }
            }
            if (col > 0) {
                off = anchor;
                rd = buf.reader(off);
            } else if (!positioned) {
                rd = buf.line_reader(line, &off);
            }
            end = fill_row(rd, font, first_col, wide, dst, &positioned, col, off, &rec, tab, sc);
        }

        // Cells holding text; everything past the end of the line is blank.
        u32 filled = end == kNoEnd ? wide : (end > first_col ? std::min(end - first_col, wide) : 0);
        if (lig) {
            font.ligate(dst, filled);
            filled = filled > back ? std::min(filled - back, cols) : 0;
            std::memcpy(row, dst + back, static_cast<size_t>(filled) * sizeof(u32));
        } else {
            filled = std::min(filled, cols);
        }
        std::memset(row + filled, 0, static_cast<size_t>(cols - filled) * sizeof(u32));

        if (g.has_sel && line >= g.sel_line0 && line <= g.sel_line1) {
            // Past the last character, one extra cell stands for the newline.
            u64 c0 = line == g.sel_line0 ? g.sel_col0 : 0;
            u64 c1 = line == g.sel_line1 ? g.sel_col1 : (end == kNoEnd ? ~0ull : u64(end) + 1);
            u64 lo = std::max<u64>(c0, g.first_col), hi = std::min<u64>(c1, u64(g.first_col) + cols);
            for (u64 c = lo; c < hi; ++c) row[c - g.first_col] |= kCellSelected;
        }
    }
}

namespace {

// Columns of white space a line starts with; -1 if it holds nothing else.
i32 line_indent(const Buffer& buf, u64 line, u32 tab) {
    if (line >= buf.line_count()) return -1;
    Buffer::Reader rd = buf.line_reader(line);
    u32 col = 0;
    for (;;) {
        const std::string_view c = rd.chunk();
        if (c.empty()) return -1;
        for (const char b : c) {
            if (b == ' ') ++col;
            else if (b == '\t') col = (col / tab + 1) * tab;
            else if (b == '\n') return -1;
            else if (b != '\r') return static_cast<i32>(col);
            if (col > 4096) return static_cast<i32>(col);
        }
        rd.advance(static_cast<u32>(c.size()));
    }
}

}  // namespace

// Indent lines. A line gets one for every level it is indented by; a blank
// line gets those of the lines around it, so that the lines of a block run
// through the gaps in it. How far each row is indented is read off the cells
// fill_grid() just made (leading spaces are cells holding a space, and past
// the end of a line a cell is 0); only when the view is scrolled sideways,
// and that part of the line is not in the cells, is the text itself read.
void guide_grid(const Buffer& buf, const GridSpec& g, u32* cells, GuideCache& gc) {
    if (!g.guide_step || !g.guide_inks || !g.rows) return;
    const u32 lead = std::min(g.lead, g.cols), cols = g.cols - lead;
    const u32 tab = std::max<u32>(1, g.tab_width);
    const u64 lines = buf.line_count();
    if (gc.doc != g.doc || gc.revision != g.revision || gc.top_line != g.top_line || gc.rows != g.rows || gc.tab_width != tab) {
        gc.doc = g.doc;
        gc.revision = g.revision;
        gc.top_line = g.top_line;
        gc.rows = g.rows;
        gc.tab_width = tab;
        gc.indent.resize(g.rows);
        gc.next.resize(g.rows);
        for (u32 r = 0; r < g.rows; ++r) {
            const u64 line = g.top_line + r;
            if (line >= lines) {
                gc.indent[r] = -1;
            } else if (g.first_col == 0) {
                const u32* row = cells + static_cast<size_t>(r) * g.cols + lead;
                u32 n = 0;
                while (n < cols && (row[n] & 0xFFFFu) == ' ') ++n;
                gc.indent[r] = n < cols && (row[n] & 0xFFFFu) == 0 ? -1 : static_cast<i32>(n);
            } else {
                gc.indent[r] = line_indent(buf, line, tab);
            }
        }
        // A blank row at an edge takes after lines that are off screen.
        constexpr u64 kLook = 200;
        gc.above = gc.below = -1;
        if (gc.indent[0] < 0)
            for (u64 l = g.top_line, n = 0; l > 0 && n < kLook && gc.above < 0; --l, ++n) gc.above = line_indent(buf, l - 1, tab);
        if (gc.indent[g.rows - 1] < 0)
            for (u64 l = g.top_line + g.rows, n = 0; l < lines && n < kLook && gc.below < 0; ++l, ++n) gc.below = line_indent(buf, l, tab);
        i32 below = gc.below;
        for (u32 r = g.rows; r-- > 0;) {
            gc.next[r] = below;
            if (gc.indent[r] >= 0) below = gc.indent[r];
        }
    }
    const u32 step = g.guide_step;
    i32 above = gc.above;
    for (u32 r = 0; r < g.rows && g.top_line + r < lines; ++r) {
        const i32 own = gc.indent[r];
        u32 levels = 0;
        if (own >= 0) {
            levels = (static_cast<u32>(own) + step - 1) / step;
            above = own;
        } else if (above >= 0 && gc.next[r] >= 0) {
            const u32 a = static_cast<u32>(above), b = static_cast<u32>(gc.next[r]);
            levels = a == b ? (a + step - 1) / step : 1 + std::min(a, b) / step;
            if (a == 0 && b == 0) levels = 0;
        }
        u32* row = cells + static_cast<size_t>(r) * g.cols + lead;
        for (u32 k = 0; k < levels; ++k) {
            const u64 col = u64(k) * step;
            if (col < g.first_col) continue;
            if (col - g.first_col >= cols) break;
            u32& cell = row[col - g.first_col];
            cell = (cell & ~((31u << kCellInkShift) | kCellDecoMask)) | ((kInkGuide + k % g.guide_inks) << kCellInkShift) |
                   (kDecoGuide << kCellDecoShift) | kCellOverlay;
        }
    }
}

// Kept out of fill_grid() so that a frame with nothing to decorate, which is
// nearly every frame, pays nothing for the possibility.
void decorate_grid(const GridSpec& g, u32* cells) {
    const u32 lead = std::min(g.lead, g.cols);
    const u64 cols = g.cols - lead, first = g.first_col;
    auto row_of = [&](u64 line) -> u32* {
        if (line < g.top_line || line >= g.top_line + g.rows) return nullptr;
        return cells + static_cast<size_t>(line - g.top_line) * g.cols + lead;
    };
    // Selections that are no run of text (a rectangle) come first. They
    // cover a line's text and no more, and the text ends where the cells
    // stop holding glyphs: only the notes below put any after it.
    for (const GridDeco& d : g.decos) {
        if (d.kind != kDecoSelect) continue;
        u32* row = row_of(d.line);
        if (!row) continue;
        const u64 lo = std::max<u64>(d.col0, first);
        u64 hi = std::min<u64>(d.col1, first + cols);
        while (hi > lo && (row[hi - 1 - first] & 0xFFFFu) == 0) --hi;
        for (u64 c = lo; c < hi; ++c) row[c - first] |= kCellSelected;
    }
    for (const GridNote& n : g.notes) {
        u32* row = row_of(n.line);
        if (!row) continue;
        for (u32 k = 0; k < n.count; ++k) {
            const u64 c = u64(n.col) + k;
            if (c >= first && c < first + cols) row[c - first] = (row[c - first] & kCellSelected) | n.cells[k];
        }
    }
    for (const GridDeco& d : g.decos) {
        u32* row = row_of(d.line);
        if (!row) continue;
        if (d.kind == kDecoSelect) continue;
        const u64 lo = std::max<u64>(d.col0, first), hi = std::min<u64>(d.col1, first + cols);
        for (u64 c = lo; c < hi; ++c) {
            u32& cell = row[c - first];
            // Where two overlap, the more serious one shows.
            const u32 have = (cell & kCellDecoMask) >> kCellDecoShift;
            if (!have || d.kind < have) cell = (cell & ~kCellDecoMask) | (d.kind << kCellDecoShift) | kCellOverlay;
        }
    }
}

void round_selection(const GridSpec& g, u32* cells) {
    const u32 lead = std::min(g.lead, g.cols), cols = g.cols - lead;
    if (!cols || !g.rows) return;
    // The run of selected cells on each row: [a, b) in the row's cells, or
    // none. Found from where the selection says it starts on that row, by
    // walking along it.
    struct Run {
        u32 a = 0, b = 0;
    };
    static thread_local std::vector<Run> runs;
    runs.assign(g.rows, Run{});
    auto find = [&](u64 line, u64 col) {
        if (line < g.top_line || line >= g.top_line + g.rows) return;
        const u32 r = static_cast<u32>(line - g.top_line);
        const u32* row = cells + static_cast<size_t>(r) * g.cols + lead;
        u32 a = col > g.first_col ? static_cast<u32>(std::min<u64>(col - g.first_col, cols)) : 0;
        while (a < cols && !(row[a] & kCellSelected)) ++a;  // (a block starts at the line's text, which may be further in)
        u32 b = a;
        while (b < cols && (row[b] & kCellSelected)) ++b;
        if (b > a) runs[r] = {a, b};
    };
    if (g.has_sel)
        for (u64 line = std::max(g.sel_line0, g.top_line); line <= g.sel_line1 && line < g.top_line + g.rows; ++line) find(line, line == g.sel_line0 ? g.sel_col0 : 0);
    for (const GridDeco& d : g.decos)
        if (d.kind == kDecoSelect) find(d.line, d.col0);

    auto covers = [&](i64 r, u32 c) { return r >= 0 && r < static_cast<i64>(g.rows) && c >= runs[static_cast<size_t>(r)].a && c < runs[static_cast<size_t>(r)].b; };
    constexpr u32 kTopLeft = 1u << kCellCornerShift, kTopRight = 2u << kCellCornerShift, kBottomLeft = 4u << kCellCornerShift, kBottomRight = 8u << kCellCornerShift;
    for (u32 r = 0; r < g.rows; ++r) {
        const Run run = runs[r];
        if (run.b <= run.a) continue;
        u32* row = cells + static_cast<size_t>(r) * g.cols + lead;
        const i64 up = static_cast<i64>(r) - 1, down = static_cast<i64>(r) + 1;
        // (A run cut off by the edge of the view goes on out of sight: no corner there.)
        const bool starts = run.a > 0 || g.first_col == 0, ends = run.b < cols;
        // Its own end cells: a corner is rounded off where the row above (or below) is not selected there.
        if (starts) row[run.a] |= (covers(up, run.a) ? 0 : kTopLeft) | (covers(down, run.a) ? 0 : kBottomLeft);
        if (ends) row[run.b - 1] |= (covers(up, run.b - 1) ? 0 : kTopRight) | (covers(down, run.b - 1) ? 0 : kBottomRight);
        // The cells just past its ends: a corner is filled in where the row above (or below) is selected there.
        if (run.a > 0) row[run.a - 1] |= (covers(up, run.a - 1) ? kTopRight : 0) | (covers(down, run.a - 1) ? kBottomRight : 0);
        if (ends) row[run.b] |= (covers(up, run.b) ? kTopLeft : 0) | (covers(down, run.b) ? kBottomLeft : 0);
    }
}

void mark_runs(u32* cells, u32 cols, u32 rows) {
    constexpr u32 kLit = kCellSelected | kCellHover;
    for (u32 r = 0; r < rows; ++r) {
        u32* row = cells + static_cast<size_t>(r) * cols;
        u32 before = 0;
        for (u32 c = 0; c < cols; ++c) {
            const u32 lit = row[c] & kLit;
            if (lit) {
                if (lit != before) row[c] |= kCellRunStart;
                if (c + 1 == cols || (row[c + 1] & kLit) != lit) row[c] |= kCellRunEnd;
            }
            before = lit;
        }
    }
}

}  // namespace blit
