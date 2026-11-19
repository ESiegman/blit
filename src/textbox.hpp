#pragma once
// Text as cells: what popups and the side panel are made of.
#include <string_view>
#include <vector>

#include "font.hpp"
#include "grid.hpp"
#include "utf8.hpp"

namespace blit {

// Display width of UTF-8 text, in cells: as the text counts them, so none
// for an emoji joined to the one before it, or a flag's second letter
// (see Joiner::width).
inline u32 width_of(std::string_view s) {
    u32 w = 0;
    Joiner j;
    const u8* p = reinterpret_cast<const u8*>(s.data());
    for (size_t i = 0; i < s.size();) {
        const Decoded d = decode_utf8(p + i, s.size() - i);
        w += j.width(d.cp);
        i += d.len;
    }
    return w;
}

// The cells of one popup.
struct TextBox {
    std::vector<u32> cells;
    u32 cols = 0, rows = 0;

    void resize(u32 c, u32 r) {
        cols = c;
        rows = r;
        cells.assign(static_cast<size_t>(c) * r, 0);
    }
    // Writes UTF-8 text at (row, col), clipped to the box. Returns the
    // column after it. Each character takes the cells it takes in the text
    // (fill_grid): a mark goes over the character before it, over both
    // cells of a wide one; an emoji sequence takes its first emoji's two.
    u32 put(Font& font, u32 row, u32 col, std::string_view s, u32 ink, u32 style = 0) {
        if (row >= rows) return col;
        if (style && !font.ensure_style(style)) style = 0;
        const u8* p = reinterpret_cast<const u8*>(s.data());
        u32* line = cells.data() + static_cast<size_t>(row) * cols;
        // The character a mark goes over: its first cell, and whether it is
        // two cells wide. (Before the text, the cell before it, as one.)
        bool base = col > 0 && col <= cols, wide = false;
        u32 at = col - 1;
        Joiner j;
        for (size_t i = 0; i < s.size();) {
            const Decoded d = decode_utf8(p + i, s.size() - i);
            i += d.len;
            const u32 cp = d.cp < 0x20 || d.cp == 0x7F ? ' ' : d.cp;
            if (j.width(cp) == 0 && cp >= 0x300) {  // (no cell of its own)
                // An emoji after a joiner, a flag's second letter, a joiner,
                // a selector, a skin tone: nothing to draw (the cluster shows
                // as its first). A mark: over the character before.
                if (cp_width(cp) != 0 || cp == 0x200D || cp - 0xFE00 < 16 || cp - 0x1F3FB < 5 || !base) continue;
                if (!wide) {
                    line[at] = (line[at] & ~0xFFFFu) | font.mark(line[at], cp);
                    continue;
                }
                line[at] = (line[at] & ~0xFFFFu) | font.mark(line[at], cp, 1);
                if (at + 1 < cols) line[at + 1] = (line[at + 1] & ~0xFFFFu) | font.mark(line[at + 1], cp, 2);
                continue;
            }
            if (col >= cols) break;
            // An emoji with more after it (a family joined by zero width
            // joiners, a skin tone, a flag's second letter): all of it read
            // first, and drawn as the colour font's picture of the most of
            // it that it has one of (as the text has it), in its first's
            // two cells.
            const u32 cg = cp < 0x80 ? cp : font.cell_glyph(cp);
            const bool ri = (cg & (Font::kWide | Font::kJoin)) == (Font::kWide | Font::kJoin);
            if ((cg & Font::kWide) && ((cg & Font::kPict) || ri) && col + 1 < cols) {
                u32 seq[12] = {cp};
                u32 n = 1, slot = 0;
                size_t k = i;
                bool joined = false;
                while (k < s.size() && n < std::size(seq)) {
                    const Decoded e = decode_utf8(p + k, s.size() - k);
                    const u32 c = e.cp, ce = c < 0x80 ? c : font.cell_glyph(c);
                    const bool next_ri = (ce & (Font::kWide | Font::kJoin)) == (Font::kWide | Font::kJoin);
                    if (!(c == 0x200D || c - 0xFE00 < 16 || c - 0x1F3FB < 5 || (joined && (ce & Font::kPict)) || (ri && next_ri && n == 1))) break;
                    joined = c == 0x200D;
                    seq[n++] = c;
                    k += e.len;
                    if (c != 0x200D && c - 0xFE00 >= 16)
                        if (const u32 whole = font.sequence_glyph(seq, n)) slot = whole;
                }
                if (slot) {
                    for (u32 m = 1; m < n; ++m) j.width(seq[m]);
                    base = true, wide = true, at = col;
                    line[col++] = slot | (ink << kCellInkShift);
                    line[col++] = (slot + 1) | (ink << kCellInkShift);
                    i = k;
                    continue;
                }
            }
            const u32 g = font.glyph(cp, style);
            const u32 slot = g & (Font::kWide - 1);
            base = true, wide = (g & Font::kWide) != 0, at = col;
            line[col++] = slot | (ink << kCellInkShift);
            if (wide && col < cols) line[col++] = (slot + 1) | (ink << kCellInkShift);
        }
        return col;
    }
    void select_row(u32 row) {
        if (row >= rows) return;
        for (u32 c = 0; c < cols; ++c) cells[static_cast<size_t>(row) * cols + c] |= kCellSelected;
    }
};

}  // namespace blit
